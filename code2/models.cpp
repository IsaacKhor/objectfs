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

/**
 * The extents code is mostly unchanged from the old implementation and was not
 * rewritten.
 */

// returns one of:
// - extent containing @offset
// - lowest extent with base > @offset
// - end()
internal_map::iterator FSFileExtentMap::lookup(int64_t offset)
{
    auto it = the_map.lower_bound(offset);
    if (it == the_map.end())
        return it;
    auto &[base, e] = *it;
    if (base > offset && it != the_map.begin()) {
        it--;
        auto &[base0, e0] = *it;
        if (offset < base0 + e0.len)
            return it;
        it++;
    }
    return it;
}

std::vector<ObjectSegment> FSFileExtentMap::find_in_range(int64_t offset,
                                                          size_t len)
{
    std::vector<ObjectSegment> ret;
    auto it = lookup(offset);
    while (it != the_map.end()) {
        auto &[base, e] = *it;
        if (base >= offset + len)
            break;
        ret.push_back(e);
        it++;
    }
    return ret;
}

/**
 * Insert a new extent into the map. If the new extent overlaps with
 * existing extents, the existing extents are chopped off to make room
 * for the new one.
 */
void FSFileExtentMap::update(int64_t offset, ObjectSegment e)
{
    // two special cases
    // (1) map is empty - just add and we're done
    //
    if (the_map.empty()) {
        the_map[offset] = e;
        return;
    }

    // extending the last extent
    //
    auto [key, val] = *(--the_map.end());
    if (offset == key + val.len && e.offset == val.offset + val.len) {
        val.len += e.len;
        the_map[key] = val;
        return;
    }

    auto it = the_map.lower_bound(offset);

    // we're at the the end of the list
    if (it == the_map.end()) {
        the_map[offset] = e;
        return;
    }

    // erase any extents fully overlapped
    //       -----  ---
    //   +++++++++++++++++
    // = +++++++++++++++++
    //
    while (it != the_map.end()) {
        auto [key, val] = *it;
        if (key >= offset && key + val.len <= offset + e.len) {
            it++;
            the_map.erase(key);
        } else
            break;
    }

    if (it != the_map.end()) {
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
            the_map.erase(key);
            the_map[new_key] = val;
        }
    }

    it = the_map.lower_bound(offset);
    if (it != the_map.begin()) {
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
            the_map[key] = val;
            val.offset += (new_key - key);
            val.len = new_len;
            the_map[new_key] = val;
        }

        // left-hand overlap
        //   ---------
        //       ++++++++++
        // = ----++++++++++
        //
        else if (key < offset && key + val.len > offset) {
            val.len = offset - key;
            the_map[key] = val;
        }
    }

    the_map[offset] = e;
}

std::vector<ObjectSegment> FSFile::segments_in_extent(int64_t offset,
                                                      size_t len)
{
    return extents->find_in_range(offset, len);
}

void FSFile::insert_segment(int64_t offset, ObjectSegment e)
{
    extents->update(offset, e);
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

inline std::vector<std::pair<std::string, inum_t>> FSDirectory::list_children()
{
    std::vector<std::pair<std::string, inum_t>> ret;
    for (auto &child : children)
        ret.push_back(child);
    return ret;
}

inline std::optional<inum_t> FSDirectory::get_child(std::string name)
{
    if (children.contains(name))
        return children.at(name);
    else
        return std::nullopt;
}
