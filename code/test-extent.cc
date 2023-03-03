#include <vector>
#include <stdint.h>

#include "extent.cc"

struct buf_extent {             // logical extent -> buffer
public:
    off_t  base;
    off_t  limit;
    char  *buf;

    static int adjacent(buf_extent a, buf_extent b) {
        return ((a.limit == b.base) &&
                (a.buf + b.base - a.base) == b.buf);
    }
    void new_base(int64_t _base) {
        buf += (_base - base);
        base = _base;
    }
    void new_limit(int64_t _limit) {
        limit = _limit;
    }
};

struct offset_extent {		// logical extent -> offset
    off_t  base;
    off_t  limit;
    int    offset;

    static int adjacent(offset_extent a, offset_extent b) {
        return ((a.limit == b.base) &&
                (a.offset + b.base - a.base) == b.offset);
    }
    void new_base(int64_t _base) {
        offset += (_base - base);
        base = _base;
    }
    void new_limit(int64_t _limit) {
        limit = _limit;
    }
};

struct plain_extent {		// logical extent. period.
public:
    off_t  base;
    off_t  limit;

    static int adjacent(plain_extent a, plain_extent b) {
        return (a.limit == b.base);
    }
    void new_base(int64_t _base) {
        base = _base;
    }
    void new_limit(int64_t _limit) {
        limit = _limit;
    }
};


void merge_extents(std::vector<offset_extent> &in_exts,
		   char *in_buf,
		   std::vector<offset_extent> &out_exts,
		   char *out_buf)
{
    extmap<buf_extent> map1;
    extmap<plain_extent> map2;

    for (auto it = in_exts.begin(); it != in_exts.end(); it++) {
	map1.update((buf_extent){.base = it->base,
				 .limit = it->limit,
				 .buf = in_buf});
	in_buf += (it->limit - it->base);
	map2.update((plain_extent){.base = it->base, .limit = it->limit});
    }

    for (auto e = map1.first(); e != nullptr; e = map1.next(e)) {
	memcpy(out_buf, e->buf, (e->limit - e->base));
	out_buf += (e->limit - e->base);
    }
    int offset = 0;
    for (auto e = map2.first(); e != nullptr; e = map2.next(e)) {
	out_exts.push_back((offset_extent){.base = e->base,
					   .limit = e->limit,
					   .offset = offset});
	offset += (e->limit - e->base);
    }
}

		   
