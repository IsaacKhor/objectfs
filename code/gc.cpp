#include "objectfs.hpp"
#include "utils.hpp"

const double GC_THRESHOLD = 0.5;

void ObjectFS::checkpoint()
{
    // no-op for now
    // TODO use libboost serialize to write the inodes map to a checkpoint obj
}

/**
 * For now, use the simplest possible GC strategy: go through the object, and
 * append all live entries to the active log. We can worry about optimisations
 * later.
 *
 * For each object we process this way, we add object to the pending list
 * to be deleted upon the next checkpoint. This way the old and new objects
 * co-exist until we're sure the new object is safe.
 */
void ObjectFS::garbage_collect()
{
    auto objs = s3.list("objectfs_");
    for (auto &obj : objs) {
        if (!should_continue_gc.load())
            return;

        auto objid = obj_backend.parse_obj_name(obj.key).value();

        // We already marked the object as collectable, just haven't gotten
        // around to deleting it yet
        if(obj_backend.is_collectable(objid))
            continue;

        auto [log_entries, backing] = obj_backend.fetch_and_parse_object(obj);

        // We do 2 passes: first we calculate the fraction of live bytes in the
        // entire object, then we GC it if it's below the threshold in a second
        // pass through the object

        auto live_bytes = calculate_live_bytes(objid, log_entries);
        auto live_frac = (double)live_bytes / (double)obj.size;
        trace("GC: object {} max live_frac={}", objid, live_frac);
        if (live_frac >= GC_THRESHOLD)
            continue;

        save_live_entries(objid, log_entries);
        trace("GC: object {} marked as collectable", objid);
        obj_backend.add_collected_obj(objid);
    }
}

/**
 * This calculates the fraction of the object occupied by live entries.
 */
size_t ObjectFS::calculate_live_bytes(objectid_t oid,
                                      std::vector<LogObjectVar> obj)
{
    size_t live_bytes = 0;
    for (auto &entry : obj) {
        if (!std::holds_alternative<LogSetFileData *>(entry))
            continue;

        auto le = std::get<LogSetFileData *>(entry);
        auto file_opt = inodes.get_copy(le->inode_num);

        // File has been deleted, it's dead
        if (!file_opt.has_value())
            continue;

        auto fsobj = file_opt.value();
        auto &file = fsobj->get_file();
        {
            std::shared_lock lock(fsobj->mtx);
            auto live = file.get_live_range(
                le->file_offset,
                ObjectSegment{oid, le->data_obj_offset, le->data_len});

            for (auto &[_, len] : live)
                live_bytes += len;
        }
    }

    return live_bytes;
}

void ObjectFS::save_live_entries(objectid_t objid,
                                 std::vector<LogObjectVar> obj)
{
    for (auto &entry : obj) {
        // Only one entry matters: SetFileData. All others are irrelevant
        // as we serialise the inode map in checkpoints and can be safely
        // collected
        if (!std::holds_alternative<LogSetFileData *>(entry))
            continue;

        auto le = std::get<LogSetFileData *>(entry);
        auto file_opt = inodes.get_copy(le->inode_num);

        // File has been deleted, it's dead
        if (!file_opt.has_value())
            continue;

        auto fsobj = file_opt.value();
        auto &file = fsobj->get_file();
        {
            std::shared_lock lock(fsobj->mtx);
            auto live = file.get_live_range(
                le->file_offset,
                ObjectSegment{objid, le->data_obj_offset, le->data_len});

            for (auto &[file_offset, len] : live) {
                LogSetFileData new_lo = {
                    .type = LogObjectType::SetFileData,
                    .inode_num = le->inode_num,
                    .file_offset = file_offset,
                    .data_obj_id = 0,     // placeholder
                    .data_obj_offset = 0, // placeholder
                    .data_len = len,
                };

                // TODO actually modify the file extents

                ASSERT(file_offset + len <= le->data_len, "live range too big");

                auto live_obj_offset =
                    &le->data[0] + file_offset - le->file_offset;
                obj_backend.append_logobj(new_lo, len, live_obj_offset);
            }
        }
    }
}

std::vector<std::pair<size_t, size_t>>
FSFile::get_live_range(size_t file_offset, ObjectSegment seg)
{
    auto [oid, obj_offset, len] = seg;
    std::vector<std::pair<size_t, size_t>> ret;

    auto in_range = segments_in_range(file_offset, len);
    for (auto [ir_file_offset, ir_seg] : in_range) {
        auto [ir_oid, ir_obj_offset, ir_len] = ir_seg;

        // The active segment is in a different object, so ours is dead
        if (ir_oid != oid)
            continue;

        // If it's in the same object, then it's live iff the segment is wholly
        // contained in our segment. This is because if there's an overwrite
        // the new data must neccesarily be appended to a new portion of the
        // log and not overwrite our entry.
        if (ir_obj_offset < obj_offset ||
            ir_obj_offset + ir_len > obj_offset + len)
            continue;

        // Segment is live
        ret.push_back({ir_file_offset, ir_len});
    }

    return ret;
}
