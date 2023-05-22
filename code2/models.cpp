#include "models.hpp"

FSObject::FSObject(inum_t inode_num, mode_t perms)
    : inode_num(inode_num), permissions(perms)
{
    owner_id = getuid();
    group_id = getgid();
}

std::unique_ptr<FSObject> FSObject::create_file(inum_t inode_num, mode_t perms)
{
    auto fo = std::make_unique<FSObject>(inode_num, perms);
    fo->data.emplace<FSFile>();
    return fo;
}

std::unique_ptr<FSObject>
FSObject::create_directory(inum_t parent_inum, inum_t self_inum, mode_t perms)
{
    auto fo = std::make_unique<FSObject>(self_inum, perms);
    fo->data.emplace<FSDirectory>(parent_inum);
    return fo;
}

std::vector<std::pair<int64_t, ObjectSegment>>
FSFile::segments_in_range(int64_t range_offset, size_t range_len)
{
    std::vector<std::pair<int64_t, ObjectSegment>> res;
    if (extents_map.empty())
        return res;

    auto it = extents_map.lower_bound(range_offset);

    // File does not contain any such segment
    if (it == extents_map.end())
        return res;

    // lower bound gives us >= offset, so we want to go back to the 1st element
    // that is <= offset and then iterate forward
    if (it->first > range_offset && it != extents_map.begin())
        it--;

    while (it != extents_map.end()) {
        auto [extent_offset, extent_segment] = *it;
        if (extent_offset >= range_offset + range_len)
            break;

        res.push_back(*it);
        it++;
    }

    // Adjust first and last segment to match the range we're looking for
    // in the case that the range starts or ends in the middle of an extent
    auto &[front_offset, front_seg] = res.front();
    auto front_adjust = range_offset - front_offset;
    front_seg.offset += front_adjust;
    front_seg.len -= front_adjust;

    auto &end_seg = res.back().second;
    if (end_seg.offset + end_seg.len > range_offset + range_len)
        end_seg.len = range_offset + range_len - end_seg.offset;

    return res;
}

void FSFile::insert_segment(int64_t offset, ObjectSegment e)
{
    /**
     * Implementation copied directly from old implementation with no changes
     */
    // two special cases
    // (1) map is empty - just add and we're done
    //
    if (extents_map.empty()) {
        extents_map[offset] = e;
        return;
    }

    // extending the last extent
    //
    auto [key, val] = *(--extents_map.end());
    if (offset == key + val.len && e.offset == val.offset + val.len) {
        val.len += e.len;
        extents_map[key] = val;
        return;
    }

    auto it = extents_map.lower_bound(offset);

    // we're at the the end of the list
    if (it == extents_map.end()) {
        extents_map[offset] = e;
        return;
    }

    // erase any extents fully overlapped
    //       -----  ---
    //   +++++++++++++++++
    // = +++++++++++++++++
    //
    while (it != extents_map.end()) {
        auto [key, val] = *it;
        if (key >= offset && key + val.len <= offset + e.len) {
            it++;
            extents_map.erase(key);
        } else
            break;
    }

    if (it != extents_map.end()) {
        // update right-hand overlap
        //        ---------
        //   ++++++++++
        // = ++++++++++----
        //
        auto [key, val] = *it;

        if (key < offset + e.len) {
            auto new_key = offset + e.len;
            val.len -= (new_key - key);
            val.offset += (new_key - key);
            extents_map.erase(key);
            extents_map[new_key] = val;
        }
    }

    it = extents_map.lower_bound(offset);
    if (it != extents_map.begin()) {
        it--;
        auto [key, val] = *it;

        // we bisect an extent
        //   ------------------
        //           +++++
        // = --------+++++-----
        if (key < offset && key + val.len > offset + e.len) {
            auto new_key = offset + e.len;
            auto new_len = val.len - (new_key - key);
            val.len = offset - key;
            extents_map[key] = val;
            val.offset += (new_key - key);
            val.len = new_len;
            extents_map[new_key] = val;
        }

        // left-hand overlap
        //   ---------
        //       ++++++++++
        // = ----++++++++++
        //
        else if (key < offset && key + val.len > offset) {
            val.len = offset - key;
            extents_map[key] = val;
        }
    }

    extents_map[offset] = e;
}

ssize_t FSFile::truncate_to(size_t new_size)
{
    for (auto &[offset, segment] : extents_map) {
        if (offset >= new_size) {
            extents_map.erase(offset);
            continue;
        }

        if (offset + segment.len > new_size) {
            segment.len = new_size - offset;
            extents_map[offset] = segment;
        }
    }
    return 0;
}

size_t FSFile::size()
{
    if (extents_map.empty())
        return 0;

    auto last = *std::prev(extents_map.end());
    return last.first + last.second.len;
}

void FSDirectory::add_child(std::string name, inum_t inum)
{
    children[name] = inum;
}

void FSDirectory::remove_child(std::string name) { children.erase(name); }

void FSDirectory::remove_child(inum_t child_inum)
{
    for (auto &[name, inum] : children)
        if (inum == child_inum) {
            children.erase(name);
            return;
        }
}

std::vector<std::pair<std::string, inum_t>> FSDirectory::list_children()
{
    std::vector<std::pair<std::string, inum_t>> ret;
    for (auto &child : children)
        ret.push_back(child);
    return ret;
}

std::optional<inum_t> FSDirectory::get_child(std::string name)
{
    if (children.contains(name))
        return std::make_optional(children.at(name));
    else
        return std::nullopt;
}
