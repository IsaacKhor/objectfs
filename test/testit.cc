#include <iostream>
#include <map>
#include <stdint.h>

struct extent {
    int64_t objnum;
    uint32_t offset;
    uint32_t len;
};

typedef std::map<int64_t, extent> internal_map;

class extmap
{
    internal_map the_map;

  public:
    internal_map::iterator begin() { return the_map.begin(); }
    internal_map::iterator end() { return the_map.end(); }

    // returns one of:
    // - extent containing @offset
    // - lowest extent with base > @offset
    // - end()
    internal_map::iterator lookup(int64_t offset)
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

    void update(int64_t offset, extent e)
    {
        auto it = the_map.lower_bound(offset);

        // we're at the the end of the list
        if (it == end()) {
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
};

int main(int argc, char **argv)
{
    int64_t results[100000];
    for (int i = 0; i < 100000; i++)
        results[i] = -1;
    extmap m;

    int n = atoi(argv[1]);
    int n2 = 100000000;
    if (argc > 2)
        n2 = atoi(argv[2]);

    for (int i = 0; i < n; i++) {
        uint32_t addr = rand() % 99000;
        uint32_t offset = rand() % 1000000;
        uint32_t len = 1 + rand() % 1000;
        extent e = {0, offset, len};
        m.update(addr, e);
        for (int j = 0; j < len; j++)
            results[addr + j] = offset + j;
        if (i > n2) {
            printf("%d..%d (+%d) = %d\n", addr, addr + len - 1, len, offset);
            for (auto it = m.begin(); it != m.end(); it++) {
                auto [key, val] = *it;
                printf("  %d..%d (+%d) -> %d\n", (int)key,
                       (int)key + val.len - 1, val.len, val.offset);
            }
        }
    }

    if (argc > 2)
        exit(1);

    auto failed = false;
    int i = 0;
    for (auto it = m.begin(); it != m.end(); it++) {
        auto [key, val] = *it;
        for (; i < key; i++) {
            if (results[i] != -1) {
                failed = true;
                std::cout << i << "map = -1 array =" << results[i] << std::endl;
            }
        }
        if (val.len > 1000) {
            printf("%d error: bad length: %d (%x)\n", i, val.len, val.len);
            failed = true;
        } else
            for (int j = 0; j < val.len; j++, i++) {
                if (results[i] != val.offset + j) {
                    printf("%d: map = %d results = %d\n", i, val.offset + j,
                           (int)results[i]);
                    failed = true;
                }
            }
    }

    if (failed)
        printf("FAILED\n");
    else
        printf("OK\n");
}
