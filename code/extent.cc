#include <cstddef>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <vector>
#include <list>

extern "C" struct rb_node *rb_next(const struct rb_node *);
extern "C" struct rb_node *rb_first(const struct rb_root *);
extern "C" void rb_insert_color(struct rb_node *, struct rb_root *);
extern "C" struct rb_node *rb_prev(const struct rb_node *);
extern "C" void rb_erase(struct rb_node *, struct rb_root *);

#include "rbtree.h"

#define TRIM_OFFSET 0xFFFFFFFE


//https://stackoverflow.com/questions/40851092/container-of-macro-in-c-with-the-same-signature-as-cs

#undef container_of
template<class P, class M>
size_t my_offsetof(const M P::*member)
{
    return (size_t) &( reinterpret_cast<P*>(0)->*member);
}
template<class P, class M>
P* my_container_of_impl(M* ptr, const M P::*member)
{
    return (P*)( (char*)ptr - my_offsetof(member));
}
#define container_of(ptr, type, member) \
     my_container_of_impl (ptr, &type::member)

template <class T>
struct extmap {
    class _extent {
    public:
	struct rb_node rb;
	T              ext;
	_extent(T _ext) {
	    ext = _ext;
	    rb_init_node(&rb);
	}
	T &val(void) {
	    return ext;
	}
    };
    
    struct rb_root r;

    // avoid container_of by ensuring rb is at start of struct
    //
    _extent *map_find_geq(int64_t lba) {
	rb_root *root = &r;
	struct rb_node *node = root->rb_node;  /* top of the tree */
	_extent *higher = NULL;
	while (node) {
	    _extent *e = (_extent*)node; // rb is at top of struct
	    if (e->ext.base >= lba &&
		(!higher || e->ext.base < higher->ext.base))
		higher = e;
	    if (lba < e->ext.base) 
		node = node->rb_left;
	    else if (lba >= e->ext.limit) 
		node = node->rb_right;
	    else 
		return e;
	}
	return higher;
    }
    
    void map_insert(_extent *_new) {
	struct rb_root *root = &r;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	_extent *e = NULL;

	RB_CLEAR_NODE(&_new->rb);

	/* Go to the bottom of the tree */
	while (*link) {
	    parent = *link;
	    e = (_extent*)parent; // rb is at top of struct
	    if (_new->ext.base < e->ext.base) {
		link = &(*link)->rb_left;
	    } else {
		link = &(*link)->rb_right;
	    }
	}
	/* Put the new node there */
	rb_link_node(&_new->rb, parent, link);
	rb_insert_color(&_new->rb, root);
    }

    _extent *map_remove(_extent *e) {
	struct rb_root *root = &r;
	struct rb_node *tmp = rb_next((struct rb_node*)e);
	rb_erase((struct rb_node*)e, root);
	return (_extent*)tmp;
    }

    _extent *map_next(_extent *e) {
	if (e == NULL)
	    return NULL;
	struct rb_node *node = rb_next((struct rb_node*)e);
	return (_extent*)node;
    }

    _extent *map_prev(_extent *e) {
	if (e == NULL)
	    return NULL;
	struct rb_node *node = rb_prev((struct rb_node*)e);
	return (_extent*)node;
    }

    _extent *map_first(void) {
	struct rb_root *root = &r;
	return (_extent*)rb_first(root);
    }

public:
    extmap(){r = RB_ROOT;}
    ~extmap(){
	_extent *e = map_first();
	while (e != NULL) {
	    _extent *tmp = map_next(e);
	    rb_erase((struct rb_node*)e, &r);
	    delete e;
	    e = tmp;
	}
    }

    T *first(void) {
	_extent *tmp = map_first();
	return tmp ? &tmp->ext : nullptr;
    }
    T *next(T *e) {
	if (!e)
	    return nullptr;
//	auto tmp = e ? (_extent *)((char*)e - sizeof(tmp->rb)) : nullptr;
	_extent *tmp = container_of(e, _extent, ext);
	tmp = map_next(tmp);
	return tmp ? &tmp->ext : nullptr;
    }
    T *prev(T *e) {
	if (!e)
	    return nullptr;
	_extent *tmp = container_of(e, _extent, ext);
	tmp = map_prev(tmp);
	return tmp ? &tmp->ext : nullptr;
    }
    T *remove(T *e) {
	if (!e)
	    return nullptr;
	_extent *tmp = container_of(e, _extent, ext);
	tmp = map_remove(tmp);
	return tmp ? &tmp->ext : nullptr;
    }
    int size(void) {
	int i = 0;
	for (auto tmp = first(); tmp != nullptr; tmp = next(tmp))
	    i++;
	return i;
    }

    void _update(T _e, bool trim) {
	struct rb_root *root = &r;
	_extent *e = map_find_geq(_e.base);

	if (e != NULL) {
	    /* [----------------------]        e     new     new2
	     *        [++++++]           -> [-----][+++++][--------]
	     */
	    if (e->ext.base < _e.base && e->ext.limit > _e.limit) {
		_extent *_new = new _extent(e->ext);
		_new->ext.new_base(_e.limit);
		e->ext.new_limit(_e.base);   /* DO THIS BEFORE INSERT */
		map_insert(_new);
		e = _new;
	    }
	    /* [------------]
	     *        [+++++++++]        -> [------][+++++++++]
	     */
	    /* never happen??? unless I change map_geq*/
	    else if (e->ext.base < _e.base && e->ext.limit <= _e.limit) {
		e->ext.new_limit(_e.base);
		e = map_next(e);
	    }
	    assert(!e || e->ext.base >= _e.base);
	    /*          [------]
	     *   [+++++++++++++++]        -> [+++++++++++++++]
	     */
	    while (e != NULL && e->ext.limit <= _e.limit) {
		_extent *tmp = map_next(e);
		map_remove(e);
		delete e;
		e = tmp;
	    }
	    /*          [------]
	     *   [+++++++++]        -> [++++++++++][---]
	     */
	    if (e != NULL && _e.limit > e->ext.base) {
		e->ext.new_base(_e.limit);
	    }
	}

	if (!trim) {
	    _extent *prev = map_prev(e);
	    if (prev && T::adjacent(prev->ext, _e)) {
		prev->ext.new_limit(_e.limit);
	    }
	    else if (e && T::adjacent(_e, e->ext)) {
		e->ext.new_base(_e.base);
	    }
	    else {
		_extent *_new = new _extent(_e);
		map_insert(_new);
	    }
	}
    }

    void update(T _e) {
	_update(_e, false);
    }
    void trim(T _e) {
	_update(_e, true);
    }

    std::vector<T> lookup(int64_t lba, int64_t limit) {
	struct rb_root *root = &r;
	_extent *e = map_find_geq(lba);
	std::vector<T> v;
	
	int i = 0;
	
	if (e && e->ext.base < lba) {
	    T _e = e->ext;
	    _e.new_base(lba);
	    if (e->ext.limit > limit)
		_e.limit = limit;
	    v.push_back(_e);
	    e = map_next(e);
	}
	for (; e && e->ext.base < limit; e = map_next(e), i++) {
	    T _e = e->ext;
	    if (limit < e->ext.limit)
		_e.limit = limit;
	    v.push_back(_e);
	}
	return v;
    }

    bool lookup(std::vector<T> &v, int64_t lba, int64_t limit) {
	bool rv = false;
	struct rb_root *root = &r;
	_extent *e = map_find_geq(lba);
	
	v.clear();
	if (e && e->ext.base < lba) {
	    rv = true;
	    T _e = e->ext;
	    _e.new_base(lba);
	    if (e->ext.limit > limit)
		_e.limit = limit;
	    v.push_back(_e);
	    e = map_next(e);
	}
	for (; e && e->ext.base < limit; e = map_next(e)) {
	    rv = true;
	    T _e = e->ext;
	    if (limit < e->ext.limit)
		_e.limit = limit;
	    v.push_back(_e);
	}
	return rv;
    }

    int iterate(std::vector<T> &v, int64_t lba, int n) {
	struct rb_root *root = &r;
	_extent *e = map_find_geq(lba);
	int i;
	v.clear();
	for (i = 0; i < n && e != nullptr; i++) {
	    v.push_back(e->ext);
	    e = map_next(e);
	}
	return i;
    }
};

