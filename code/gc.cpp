#include "objectfs.hpp"

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

        auto [log_entries, backing] = obj_backend.fetch_and_parse_object(obj);
        auto live_bytes = calculate_live_bytes(log_entries);
        auto live_frac = (double)live_bytes / (double)obj.size;

        if (live_frac >= GC_THRESHOLD)
            continue;

        save_live_entries(log_entries);
    }
}

size_t calculate_live_bytes(std::vector<LogObjectVar> obj)
{
    size_t live_bytes = 0;
    for (auto &entry : obj) {
        if (!std::holds_alternative<LogSetFileData *>(entry))
            continue;

        auto le = std::get<LogSetFileData *>(entry);
        live_bytes += le->data_len + sizeof(LogSetFileData);
    }

    return live_bytes;
}

void ObjectFS::save_live_entries(std::vector<LogObjectVar> obj)
{
    for (auto &entry : obj) {
        // Only one entry matters: SetFileData. All others are irrelevant
        // as we serialise the inode map in checkpoints and can be safely
        // collected
        if (!std::holds_alternative<LogSetFileData *>(entry))
            continue;

        auto le = std::get<LogSetFileData *>(entry);
    }
}
