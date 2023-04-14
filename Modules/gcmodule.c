/*

  Reference Cycle Garbage Collection
  ==================================

  Neil Schemenauer <nas@arctrix.com>

  Based on a post on the python-dev list.  Ideas from Guido van Rossum,
  Eric Tiedemann, and various others.

  http://www.arctrix.com/nas/python/gc/

  The following mailing list threads provide a historical perspective on
  the design of this module.  Note that a fair amount of refinement has
  occurred since those discussions.

  http://mail.python.org/pipermail/python-dev/2000-March/002385.html
  http://mail.python.org/pipermail/python-dev/2000-March/002434.html
  http://mail.python.org/pipermail/python-dev/2000-March/002497.html

  For a highlevel view of the collection process, read the collect
  function.

*/

#include "Python.h"
#include "pycore_context.h"
#include "pycore_dict.h"
#include "pycore_initconfig.h"
#include "pycore_interp.h"      // PyInterpreterState.gc
#include "pycore_object.h"
#include "pycore_pyerrors.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "pycore_refcnt.h"
#include "pycore_gc.h"
#include "frameobject.h"        /* for PyFrame_ClearFreeList */
#include "pydtrace.h"

#include "mimalloc.h"
#include "mimalloc-internal.h"

typedef struct _gc_runtime_state GCState;

/*[clinic input]
module gc
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b5c9690ecc842d79]*/


#ifdef Py_DEBUG
#  define GC_DEBUG
#endif

#define GC_NEXT _PyGCHead_NEXT
#define GC_PREV _PyGCHead_PREV

/* Get an object's GC head */
#define AS_GC(o) ((PyGC_Head *)(((char *)(o))+PyGC_Head_OFFSET))

/* Get the object given the GC head */
#define FROM_GC(g) ((PyObject *)(((char *)(g))-PyGC_Head_OFFSET))

#define _PyGC_TRACKED(g) (((g)->_gc_prev & _PyGC_PREV_MASK_TRACKED) != 0)

typedef enum {
    /* GC was triggered by heap allocation */
    GC_REASON_HEAP,

    /* GC was called due to shutdown */
    GC_REASON_SHUTDOWN,

    /* GC was called via gc.collect() or PyGC_Collect */
    GC_REASON_MANUAL
} _PyGC_Reason;

static inline void
gc_set_unreachable(PyGC_Head *g)
{
    g->_gc_prev |= _PyGC_PREV_MASK_UNREACHABLE;
}

static inline int
gc_is_unreachable(PyGC_Head *g)
{
    return (g->_gc_prev & _PyGC_PREV_MASK_UNREACHABLE) != 0;
}

static inline Py_ssize_t
gc_get_refs(PyGC_Head *g)
{
    return ((Py_ssize_t)g->_gc_prev) >> _PyGC_PREV_SHIFT;
}

static inline void
gc_set_refs(PyGC_Head *g, Py_ssize_t refs)
{
    g->_gc_prev = (g->_gc_prev & ~_PyGC_PREV_MASK)
        | ((uintptr_t)(refs) << _PyGC_PREV_SHIFT);
}

static inline void
gc_add_refs(PyGC_Head *g, Py_ssize_t refs)
{
    assert(!gc_is_unreachable(g)); // if so we should clear it???
    g->_gc_prev += (refs << _PyGC_PREV_SHIFT);
}

static inline void
gc_decref(PyGC_Head *g)
{
    g->_gc_prev -= 1 << _PyGC_PREV_SHIFT;
}

/* set for debugging information */
#define DEBUG_STATS             (1<<0) /* print collection statistics */
#define DEBUG_COLLECTABLE       (1<<1) /* print collectable objects */
#define DEBUG_UNCOLLECTABLE     (1<<2) /* print uncollectable objects */
#define DEBUG_SAVEALL           (1<<5) /* save all garbage in gc.garbage */
#define DEBUG_LEAK              DEBUG_COLLECTABLE | \
                DEBUG_UNCOLLECTABLE | \
                DEBUG_SAVEALL


static inline void gc_list_init(PyGC_Head *list);


static GCState *
get_gc_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->gc;
}


void
_PyGC_InitState(GCState *gcstate)
{
    gcstate->enabled = 1; /* automatic collection enabled? */
    gcstate->gc_live = 0;
    gcstate->gc_threshold = 7000;
    gcstate->gc_scale = 100;

    const char* scale_str = _Py_GetEnv(1, "PYTHONGC");
    if (scale_str) {
        (void)_Py_str_to_int(scale_str, &gcstate->gc_scale);
    }
}


PyStatus
_PyGC_Init(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;

    gcstate->garbage = PyList_New(0);
    if (gcstate->garbage == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    gcstate->callbacks = PyList_New(0);
    if (gcstate->callbacks == NULL) {
        return _PyStatus_NO_MEMORY();
    }

    return _PyStatus_OK();
}


/*
_gc_prev values
---------------

Between collections, _gc_prev is used for doubly linked list.

Lowest two bits of _gc_prev are used for flags.
PREV_MASK_COLLECTING is used only while collecting and cleared before GC ends
or _PyObject_GC_UNTRACK() is called.

During a collection, _gc_prev is temporary used for gc_refs, and the gc list
is singly linked until _gc_prev is restored.

gc_refs
    At the start of a collection, update_refs() copies the true refcount
    to gc_refs, for each object in the generation being collected.
    subtract_refs() then adjusts gc_refs so that it equals the number of
    times an object is referenced directly from outside the generation
    being collected.

PREV_MASK_COLLECTING
    Objects in generation being collected are marked PREV_MASK_COLLECTING in
    update_refs().


_gc_next values
---------------

_gc_next takes these values:

0
    The object is not tracked

!= 0
    Pointer to the next object in the GC list.
    Additionally, lowest bit is used temporary for
    NEXT_MASK_UNREACHABLE flag described below.

NEXT_MASK_UNREACHABLE
    move_unreachable() then moves objects not reachable (whether directly or
    indirectly) from outside the generation into an "unreachable" set and
    set this flag.

    Objects that are found to be reachable have gc_refs set to 1.
    When this flag is set for the reachable object, the object must be in
    "unreachable" set.
    The flag is unset and the object is moved back to "reachable" set.

    move_legacy_finalizers() will remove this flag from "unreachable" set.
*/

/*** list functions ***/

static inline void
gc_list_init(PyGC_Head *list)
{
    // List header must not have flags.
    // We can assign pointer by simple cast.
    list->_gc_prev = (uintptr_t)list;
    list->_gc_next = (uintptr_t)list;
}

static inline int
gc_list_is_empty(PyGC_Head *list)
{
    return (list->_gc_next == (uintptr_t)list);
}

/* Append `node` to `list`. */
static inline void
gc_list_append(PyGC_Head *node, PyGC_Head *list)
{
    PyGC_Head *last = (PyGC_Head *)list->_gc_prev;

    // last <-> node
    _PyGCHead_SET_PREV(node, last);
    _PyGCHead_SET_NEXT(last, node);

    // node <-> list
    _PyGCHead_SET_NEXT(node, list);
    list->_gc_prev = (uintptr_t)node;
}

/* Remove `node` from the gc list it's currently in. */
static inline void
gc_list_remove(PyGC_Head *node)
{
    PyGC_Head *prev = GC_PREV(node);
    PyGC_Head *next = GC_NEXT(node);

    _PyGCHead_SET_NEXT(prev, next);
    _PyGCHead_SET_PREV(next, prev);

    /* object is not currently tracked */
    // assert((node->_gc_prev & _PyGC_PREV_MASK_TRACKED) == 0);

    node->_gc_next = 0;
    node->_gc_prev &= (_PyGC_PREV_MASK_TRACKED | _PyGC_PREV_MASK_FINALIZED);
}

/* Move `node` from the gc list it's currently in (which is not explicitly
 * named here) to the end of `list`.  This is semantically the same as
 * gc_list_remove(node) followed by gc_list_append(node, list).
 */
static void
gc_list_move(PyGC_Head *node, PyGC_Head *list)
{
    /* Unlink from current list. */
    PyGC_Head *from_prev = GC_PREV(node);
    PyGC_Head *from_next = GC_NEXT(node);
    _PyGCHead_SET_NEXT(from_prev, from_next);
    _PyGCHead_SET_PREV(from_next, from_prev);

    /* Relink at end of new list. */
    // list must not have flags.  So we can skip macros.
    PyGC_Head *to_prev = (PyGC_Head*)list->_gc_prev;
    _PyGCHead_SET_PREV(node, to_prev);
    _PyGCHead_SET_NEXT(to_prev, node);
    list->_gc_prev = (uintptr_t)node;
    _PyGCHead_SET_NEXT(node, list);
}

static void
gc_list_clear(PyGC_Head *list)
{
    PyGC_Head *gc = GC_NEXT(list);
    while (gc != list) {
        PyGC_Head *next = GC_NEXT(gc);
        gc->_gc_next = 0;
        gc->_gc_prev &= ~_PyGC_PREV_MASK;
        gc = next;
    }
    gc_list_init(list);
}

static Py_ssize_t
gc_list_size(PyGC_Head *list)
{
    PyGC_Head *gc;
    Py_ssize_t n = 0;
    for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(gc)) {
        n++;
    }
    return n;
}

static Py_ssize_t
_Py_GC_REFCNT(PyObject *op)
{
    Py_ssize_t local, shared;
    int immortal;

    _PyRef_UnpackLocal(op->ob_ref_local, &local, &immortal);
    _PyRef_UnpackShared(op->ob_ref_shared, &shared, NULL, NULL);
    assert(!immortal);

    return local + shared;
}

typedef int (gc_visit_fn)(PyGC_Head* gc, void *arg);

/* True if memory is allocated by the debug allocator.
 * See obmalloc.c
 */
static int using_debug_allocator;

static int
visit_page(const mi_page_t* page, gc_visit_fn* visitor, void *arg)
{
    mi_segment_t* segment = _mi_page_segment(page);
    size_t block_size = page->xblock_size;
    uint8_t *data = _mi_page_start(segment, page, NULL);
    // printf("visiting page %p of size %zu capacity %d debug=%d\n", page, block_size, (int)page->capacity, (int)using_debug_allocator);
    for (int i = 0, end = page->capacity; i != end; i++) {
        uint8_t *p = data + i * block_size;
        if (using_debug_allocator) {
            /* The debug allocator sticks two words before each allocation.
             * When the allocation is active, the low bit of the first word
             * is set.
             */
            /* TODO(sgross): update and handle debug allocator in obmalloc.c */
            size_t *size_prefix = (size_t*)p;
            if (!(*size_prefix & 1)) {
                continue;
            }
            p += 2 * sizeof(size_t);
        }
        PyGC_Head *gc = (PyGC_Head *)p;
        if (_PyGC_TRACKED(gc)) {
            int err = (*visitor)(gc, arg);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int
visit_segments(mi_segment_t* segment, gc_visit_fn* visitor, void *arg)
{
    while (segment) {
        const mi_slice_t* end;
        mi_slice_t* slice = mi_slices_start_iterate(segment, &end);
        while (slice < end) {
            if (slice->xblock_size > 0) {
                mi_page_t* const page = mi_slice_to_page(slice);
                if (page->tag == mi_heap_tag_gc) {
                    int err = visit_page(page, visitor, arg);
                    if (err) {
                        return err;
                    }
                }
            }
            slice = slice + slice->slice_count;
        }
        segment = segment->abandoned_next;
    }
    return 0;
}

static int
visit_heap(mi_heap_t *heap, gc_visit_fn* visitor, void *arg)
{
    if (!heap || heap->visited || heap->page_count == 0) {
        return 0;
    }

    for (size_t i = 0; i <= MI_BIN_FULL; i++) {
        const mi_page_queue_t *pq = &heap->pages[i];
        mi_page_t *page = pq->first;
        while (page != NULL) {
            assert(page->tag == mi_heap_tag_gc);
            int err = visit_page(page, visitor, arg);
            if (err) {
                return err;
            }
            page = page->next;
        }
    }

    heap->visited = true;
    return 0;
}

static int
visit_heaps(gc_visit_fn* visitor, void *arg)
{
    int err = 0;
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *t;

    HEAD_LOCK(runtime);

    using_debug_allocator = _PyMem_DebugEnabled();

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[mi_heap_tag_gc];
        int err = visit_heap(heap, visitor, arg);
        if (err) {
            goto end;
        }
    }

    err = visit_segments(_mi_segment_abandoned(), visitor, arg);
    if (err) {
        goto end;
    }

    err = visit_segments(_mi_segment_abandoned_visited(), visitor, arg);
    if (err) {
        goto end;
    }

end:
    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[mi_heap_tag_gc];
        if (heap) {
            heap->visited = false;
        }
    }

    HEAD_UNLOCK(runtime);
    return err;
}

struct debug_visitor_args {
    mi_block_visit_fun *visitor;
    void *arg;
};

static bool
debug_visitor(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* arg)
{
    struct debug_visitor_args *args = (struct debug_visitor_args *)arg;
    if (block) {
        block = (char *)block + 2 * sizeof(size_t);
    }
    block_size -= 2 * sizeof(size_t);
    return args->visitor(heap, area, block, block_size, args->arg);
}

static void
visit_heaps2(int heap_tag, mi_block_visit_fun *visitor, void *arg)
{
    _PyRuntimeState *runtime = &_PyRuntime;
    PyThreadState *t;
    struct debug_visitor_args debug_args = { visitor, arg };

    HEAD_LOCK(runtime);
    using_debug_allocator = _PyMem_DebugEnabled();

    // FIXME(sgross): dict keys don't go through debug allocator
    if (using_debug_allocator && heap_tag != mi_heap_tag_dict_keys) {
        visitor = debug_visitor;
        arg = &debug_args;
    }

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[heap_tag];
        if (heap && !heap->visited) {
            mi_heap_visit_blocks(heap, true, visitor, arg);
            heap->visited = true;
        }
    }

    _mi_abandoned_visit_blocks(heap_tag, true, visitor, arg);

    for_each_thread(t) {
        mi_heap_t *heap = t->heaps[heap_tag];
        if (heap) {
            heap->visited = false;
        }
    }

    HEAD_UNLOCK(runtime);
}

struct find_object_args {
    PyObject *op;
    int found;
};

static int
find_object_visitor(PyGC_Head* gc, void *arg)
{
    struct find_object_args *args = (struct find_object_args *)arg;
    if (FROM_GC(gc) == args->op) {
        args->found = 1;
    }
    return 0;
}

int
_PyGC_find_object(PyObject *op)
{
    struct find_object_args args;
    args.op = op;
    args.found = 0;
    visit_heaps(find_object_visitor, &args);
    return args.found;
}

// Constants for validate_list's flags argument.
enum flagstates {unreachable_clear,
                 unreachable_set};

#ifdef GC_DEBUG
// validate_list checks list consistency.  And it works as document
// describing when flags are expected to be set / unset.
// `head` must be a doubly-linked gc list, although it's fine (expected!) if
// the prev and next pointers are "polluted" with flags.
// What's checked:
// - The `head` pointers are not polluted.
// - The objects' PREV_MASK_COLLECTING and NEXT_MASK_UNREACHABLE flags are all
//   `set or clear, as specified by the 'flags' argument.
// - The prev and next pointers are mutually consistent.
static void
validate_list(PyGC_Head *head, enum flagstates flags)
{
    assert(!gc_is_unreachable(head));
    uintptr_t prev_mask = 0, prev_value = 0;
    switch (flags) {
        case unreachable_clear:
            prev_mask = _PyGC_PREV_MASK_UNREACHABLE;
            prev_value = 0;
            break;
        case unreachable_set:
            prev_mask = _PyGC_PREV_MASK_UNREACHABLE;
            prev_value = _PyGC_PREV_MASK_UNREACHABLE;
            break;
        default:
            assert(! "bad internal flags argument");
    }
    PyGC_Head *prev = head;
    PyGC_Head *gc = GC_NEXT(head);
    int n = 0;
    while (gc != head) {
        PyGC_Head *trueprev = GC_PREV(gc);
        PyGC_Head *truenext = (PyGC_Head *)(gc->_gc_next);
        assert(truenext != NULL);
        assert(trueprev == prev);
        assert((gc->_gc_prev & prev_mask) == prev_value);
        assert((gc->_gc_next & 3) == 0);
        prev = gc;
        gc = truenext;
        n++;
    }
    assert(prev == GC_PREV(head));
}

static int
validate_refcount_visitor(PyGC_Head* gc, void *arg)
{
    assert(_Py_GC_REFCNT(FROM_GC(gc)) > 0);
    return 0;
}

static void
validate_refcount(void)
{
    visit_heaps(validate_refcount_visitor, NULL);
}

struct validate_tracked_args {
    uintptr_t mask;
    uintptr_t expected;
};

static int
validate_tracked_visitor(PyGC_Head* gc, void *void_arg)
{
    struct validate_tracked_args *arg = (struct validate_tracked_args*)void_arg;
    assert((gc->_gc_prev & arg->mask) == arg->expected);
    assert(gc->_gc_next == 0);
    assert(_PyGCHead_PREV(gc) == NULL);
    assert(_Py_GC_REFCNT(FROM_GC(gc)) >= 0);
    return 0;
}

static void
validate_tracked_heap(uintptr_t mask, uintptr_t expected)
{
    struct validate_tracked_args args;
    args.mask = mask;
    args.expected = expected;
    visit_heaps(validate_tracked_visitor, &args);
}
#else
#define validate_list(x, y) do{}while(0)
#define validate_refcount() do{}while(0)
#define validate_tracked_heap(x,y) do{}while(0)
#endif

static int
reset_heap_visitor(PyGC_Head *gc, void *void_arg)
{
    gc->_gc_prev = 0;
    return 0;
}

void
_PyGC_ResetHeap(void)
{
    // NOTE: _PyGC_Initialize may be called multiple times. For example,
    // _test_embed triggers multiple GC initializations, including some
    // after _Py_Initialize failures. Since _Py_Initialize clears _PyRuntime
    // we have no choice but to leak all PyObjects.
    // TODO(sgross): should we drop mi_heap here instead?
    visit_heaps(reset_heap_visitor, NULL);
}

/* Subtracts incoming references. */
static int
visit_decref(PyObject *op, void *arg)
{
    if (_PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        // We're only interested in gc_refs for tracked objects.
        if (_PyGC_TRACKED(gc)) {
            gc_decref(gc);
        }
    }
    return 0;
}

static void
find_dead_shared_keys(_PyObjectQueue **queue, int *num_unmarked)
{
    PyInterpreterState *interp = _PyRuntime.interpreters.head;
    while (interp) {
        struct _Py_dict_state *dict_state = &interp->dict_state;
        PyDictSharedKeysObject **prev_nextptr = &dict_state->tracked_shared_keys;
        PyDictSharedKeysObject *keys = dict_state->tracked_shared_keys;
        while (keys) {
            assert(keys->tracked);
            PyDictSharedKeysObject *next = keys->next;
            if (keys->marked) {
                keys->marked = 0;
                prev_nextptr = &keys->next;
                *num_unmarked += 1;
            }
            else {
                *prev_nextptr = next;
                // FIXME: bad cast
                _PyObjectQueue_Push(queue, (PyObject *)keys);
            }
            keys = next;
        }

        interp = interp->next;
    }
}

struct update_refs_args {
    PyGC_Head *list;
    int split_keys_marked;
};

// Compute the number of external references to objects in the heap
// by subtracting internal references from the refcount.
static bool
update_refs(const mi_heap_t* heap, const mi_heap_area_t* area, void* block, size_t block_size, void* args)
{
    PyGC_Head *gc = (PyGC_Head *)block;
    if (!gc) return true;

    struct update_refs_args *arg = (struct update_refs_args *)args;

    PyObject *op = FROM_GC(gc);
    if (PyDict_CheckExact(op)) {
        PyDictObject *mp = (PyDictObject *)op;
        if (mp->ma_keys && mp->ma_keys->dk_kind == DICT_KEYS_SPLIT) {
            PyDictSharedKeysObject *shared = DK_AS_SPLIT(mp->ma_keys);
            if (shared->tracked) {
                shared->marked = 1;
                arg->split_keys_marked++;
            }
        }
    }
    if (!_PyGC_TRACKED(gc)) {
        return true;
    }
    PyGC_Head *list = arg->list;

    assert(_PyGC_TRACKED(gc));

    if (PyTuple_CheckExact(op)) {
        _PyTuple_MaybeUntrack(op);
        if (!_PyObject_GC_IS_TRACKED(op)) {
            gc->_gc_prev &= ~_PyGC_PREV_MASK_FINALIZED;
            return true;
        }
    }
    else if (PyDict_CheckExact(op)) {
        _PyDict_MaybeUntrack(op);
        if (!_PyObject_GC_IS_TRACKED(op)) {
            gc->_gc_prev &= ~_PyGC_PREV_MASK_FINALIZED;
            return true;
        }
    }

    // Add the actual refcount to gc_refs.
    Py_ssize_t refcount = _Py_GC_REFCNT(op);
    _PyObject_ASSERT(op, refcount >= 0);
    gc_add_refs(gc, refcount);

    // Subtract internal references from gc_refs. Objects with gc_refs > 0
    // are directly reachable from outside containers, and so can't be
    // collected.
    Py_TYPE(op)->tp_traverse(op, visit_decref, NULL);

    PyGC_Head *prev = (PyGC_Head *)list->_gc_prev;
    prev->_gc_next = (uintptr_t)gc;
    gc->_gc_next = (uintptr_t)list;
    list->_gc_prev = (uintptr_t)gc;
    return true;
}

/* A traversal callback for subtract_refs. */
static int
visit_decref_unreachable(PyObject *op, void *data)
{
    assert(op != NULL);
    if (PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        /* We're only interested in gc_refs for objects in the
         * generation being collected, which can be recognized
         * because only they have positive gc_refs.
         */
        if (gc_is_unreachable(gc)) {
            gc_decref(gc);
        }
    }
    return 0;
}

/* Subtract internal references from gc_refs.  After this, gc_refs is >= 0
 * for all objects in containers, and is GC_REACHABLE for all tracked gc
 * objects not in containers.  The ones with gc_refs > 0 are directly
 * reachable from outside containers, and so can't be collected.
 */
static void
subtract_refs_unreachable(PyGC_Head *containers)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(containers);
    for (; gc != containers; gc = GC_NEXT(gc)) {
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        (void) traverse(FROM_GC(gc),
                       (visitproc)visit_decref_unreachable,
                       NULL);
    }
}

/* A traversal callback for move_unreachable. */
static int
visit_reachable(PyObject *op, PyGC_Head *reachable)
{
    if (!_PyObject_IS_GC(op)) {
        return 0;
    }

    PyGC_Head *gc = AS_GC(op);
    const Py_ssize_t gc_refs = gc_get_refs(gc);

    // Ignore untracked objects and objects in other generation.
    // NOTE: there is a combination of bugs we have to beware of here. After
    // a fork, we lost track of the heaps from other threads. They're not properly
    // abandoned, so visit_heap doesn't see them.
    if (gc->_gc_next == 0) {
        return 0;
    }

    if (gc_is_unreachable(gc)) {
        /* This had gc_refs = 0 when move_unreachable got
         * to it, but turns out it's reachable after all.
         * Move it back to move_unreachable's 'young' list,
         * and move_unreachable will eventually get to it
         * again.
         */
        // Manually unlink gc from unreachable list because the list functions
        // don't work right in the presence of NEXT_MASK_UNREACHABLE flags.
        PyGC_Head *prev = GC_PREV(gc);
        PyGC_Head *next = (PyGC_Head*)gc->_gc_next;

        // TODO: can't do these asserts because prev/next may be list head
        //_PyObject_ASSERT(FROM_GC(prev), gc_is_unreachable(prev));
        //_PyObject_ASSERT(FROM_GC(next), gc_is_unreachable(next));

        prev->_gc_next = gc->_gc_next;
        _PyGCHead_SET_PREV(next, prev);

        gc_list_append(gc, reachable);
        gc_set_refs(gc, 1);
        gc->_gc_prev &= ~_PyGC_PREV_MASK_UNREACHABLE;
    }
    else if (gc_refs == 0) {
        /* This is in move_unreachable's 'young' list, but
         * the traversal hasn't yet gotten to it.  All
         * we need to do is tell move_unreachable that it's
         * reachable.
         */
        assert((gc->_gc_next & ~3) != 0);
        gc_set_refs(gc, 1);
    }
    /* Else there's nothing to do.
     * If gc_refs > 0, it must be in move_unreachable's 'young'
     * list, and move_unreachable will eventually get to it.
     */
    else {
        _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(gc) > 0, "refcount is too small");
    }
    return 0;
}

/* Move the unreachable objects from young to unreachable.  After this,
 * all objects in young don't have PREV_MASK_COLLECTING flag and
 * unreachable have the flag.
 * All objects in young after this are directly or indirectly reachable
 * from outside the original young; and all objects in unreachable are
 * not.
 *
 * This function restores _gc_prev pointer.  young and unreachable are
 * doubly linked list after this function.
 * But _gc_next in unreachable list has NEXT_MASK_UNREACHABLE flag.
 * So we can not gc_list_* functions for unreachable until we remove the flag.
 */
static void
move_unreachable(PyGC_Head *young, PyGC_Head *unreachable)
{
    // previous elem in the young list, used for restore gc_prev.
    PyGC_Head *prev = young;
    PyGC_Head *gc = GC_NEXT(young);

    /* Invariants:  all objects "to the left" of us in young are reachable
     * (directly or indirectly) from outside the young list as it was at entry.
     *
     * All other objects from the original young "to the left" of us are in
     * unreachable now, and have NEXT_MASK_UNREACHABLE.  All objects to the
     * left of us in 'young' now have been scanned, and no objects here
     * or to the right have been scanned yet.
     */

    while (gc != young) {
        if (gc_get_refs(gc)) {
            /* gc is definitely reachable from outside the
             * original 'young'.  Mark it as such, and traverse
             * its pointers to find any other objects that may
             * be directly reachable from it.  Note that the
             * call to tp_traverse may append objects to young,
             * so we have to wait until it returns to determine
             * the next object to visit.
             */
            PyObject *op = FROM_GC(gc);
            traverseproc traverse = Py_TYPE(op)->tp_traverse;
            _PyObject_ASSERT_WITH_MSG(op, gc_get_refs(gc) > 0,
                                      "refcount is too small");
            // NOTE: visit_reachable may change gc->_gc_next when
            // young->_gc_prev == gc.  Don't do gc = GC_NEXT(gc) before!
            (void) traverse(op,
                    (visitproc)visit_reachable,
                    (void *)young);
            // relink gc_prev to prev element.
            _PyGCHead_SET_PREV(gc, prev);
            prev = gc;
        }
        else {
            /* This *may* be unreachable.  To make progress,
             * assume it is.  gc isn't directly reachable from
             * any object we've already traversed, but may be
             * reachable from an object we haven't gotten to yet.
             * visit_reachable will eventually move gc back into
             * young if that's so, and we'll see it again.
             */
            // Move gc to unreachable.
            // No need to gc->next->prev = prev because it is single linked.
            prev->_gc_next = gc->_gc_next;

            // We can't use gc_list_append() here because we use
            // NEXT_MASK_UNREACHABLE here.
            PyGC_Head *last = GC_PREV(unreachable);
            // NOTE: Since all objects in unreachable set has
            // NEXT_MASK_UNREACHABLE flag, we set it unconditionally.
            // But this may pollute the unreachable list head's 'next' pointer
            // too. That's semantically senseless but expedient here - the
            // damage is repaired when this function ends.
            last->_gc_next = (uintptr_t)gc;
            _PyGCHead_SET_PREV(gc, last);
            gc->_gc_next = (uintptr_t)unreachable;
            unreachable->_gc_prev = (uintptr_t)gc;
            gc_set_unreachable(gc);
            assert(last == _PyGCHead_PREV(gc));
        }
        gc = (PyGC_Head*)prev->_gc_next;
    }
    // young->_gc_prev must be last element remained in the list.
    young->_gc_prev = (uintptr_t)prev;
}

/* Return true if object has a pre-PEP 442 finalization method. */
static int
has_legacy_finalizer(PyObject *op)
{
    return Py_TYPE(op)->tp_del != NULL;
}

/* Move the objects in unreachable with tp_del slots into `finalizers`.
 *
 * This function also removes NEXT_MASK_UNREACHABLE flag
 * from _gc_next in unreachable.
 */
static void
move_legacy_finalizers(PyGC_Head *unreachable, PyGC_Head *finalizers)
{
    PyGC_Head *gc, *next;

    /* March over unreachable.  Move objects with finalizers into
     * `finalizers`.
     */
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        PyObject *op = FROM_GC(gc);

        _PyObject_ASSERT(op, gc_is_unreachable(gc));
        next = (PyGC_Head*)gc->_gc_next;

        if (has_legacy_finalizer(op)) {
            gc_list_move(gc, finalizers);
            gc->_gc_prev &= ~_PyGC_PREV_MASK_UNREACHABLE;
        }
    }
}

static inline void
clear_unreachable_mask(PyGC_Head *unreachable)
{
    /* Check that the list head does not have the unreachable bit set */
    PyGC_Head *gc, *next;
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        gc->_gc_prev &= ~_PyGC_PREV_MASK_UNREACHABLE;
        next = (PyGC_Head*)gc->_gc_next;
    }
    // validate_list(unreachable, unreachable_clear);
}

/* A traversal callback for move_legacy_finalizer_reachable. */
static int
visit_move(PyObject *op, PyGC_Head *tolist)
{
    if (_PyObject_IS_GC(op)) {
        PyGC_Head *gc = AS_GC(op);
        if (gc_is_unreachable(gc)) {
            gc_list_move(gc, tolist);
            gc->_gc_prev &= ~_PyGC_PREV_MASK_UNREACHABLE;
        }
    }
    return 0;
}

/* Move objects that are reachable from finalizers, from the unreachable set
 * into finalizers set.
 */
static void
move_legacy_finalizer_reachable(PyGC_Head *finalizers)
{
    traverseproc traverse;
    PyGC_Head *gc = GC_NEXT(finalizers);
    for (; gc != finalizers; gc = GC_NEXT(gc)) {
        /* Note that the finalizers list may grow during this. */
        traverse = Py_TYPE(FROM_GC(gc))->tp_traverse;
        (void) traverse(FROM_GC(gc),
                        (visitproc)visit_move,
                        (void *)finalizers);
    }
}

/* Adds one to the refcount and merges the local and shared fields. */
static void
incref_merge(PyObject *op)
{
    Py_ssize_t local_refcount, shared_refcount;
    int immortal;

    assert(_PyRuntime.stop_the_world);

#ifdef Py_REF_DEBUG
    _Py_IncRefTotal();
#endif

    _PyRef_UnpackLocal(op->ob_ref_local, &local_refcount, &immortal);
    _PyRef_UnpackShared(op->ob_ref_shared, &shared_refcount, NULL, NULL);
    assert(!immortal && "immortal objects should not be in garbage");

    Py_ssize_t refcount = local_refcount + shared_refcount + 1;
    op->ob_tid = 0;
    op->ob_ref_local = 0;
    op->ob_ref_shared = _Py_REF_PACK_SHARED(refcount, _Py_REF_MERGED);
}

/* Subtracts one from the refcount field. */
static void
decref_merged(PyObject *op)
{
    assert(_Py_REF_IS_MERGED(op->ob_ref_shared));
    assert(((op->ob_ref_shared) >> _Py_REF_SHARED_SHIFT) > 1);
#ifdef Py_REF_DEBUG
    _Py_DecRefTotal();
#endif
    op->ob_ref_shared -= (1 << _Py_REF_SHARED_SHIFT);
}

/* Clear all weakrefs to unreachable objects, and if such a weakref has a
 * callback, invoke it if necessary.  Note that it's possible for such
 * weakrefs to be outside the unreachable set -- indeed, those are precisely
 * the weakrefs whose callbacks must be invoked.  See gc_weakref.txt for
 * overview & some details.  Some weakrefs with callbacks may be reclaimed
 * directly by this routine; the number reclaimed is the return value.  Other
 * weakrefs with callbacks may be moved into the `old` generation.  Objects
 * moved into `old` have gc_refs set to GC_REACHABLE; the objects remaining in
 * unreachable are left at GC_TENTATIVELY_UNREACHABLE.  When this returns,
 * no object in `unreachable` is weakly referenced anymore.
 */
static void
clear_weakrefs(PyGC_Head *unreachable, PyGC_Head *wrcb_to_call)
{
    PyGC_Head *gc;
    PyObject *op;               /* generally FROM_GC(gc) */
    PyGC_Head *next;

    /* Clear all weakrefs to the objects in unreachable.  If such a weakref
     * also has a callback, move it into `wrcb_to_call` if the callback
     * needs to be invoked.  Note that we cannot invoke any callbacks until
     * all weakrefs to unreachable objects are cleared, lest the callback
     * resurrect an unreachable object via a still-active weakref.  We
     * make another pass over wrcb_to_call, invoking callbacks, after this
     * pass completes.
     */
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = next) {
        op = FROM_GC(gc);
        next = GC_NEXT(gc);

        /* Add one to the refcount to prevent deallocation while we're holding
         * on to it in a list. */
        incref_merge(op);

        if (PyWeakref_Check(op)) {
            /* A weakref inside the unreachable set must be cleared.  If we
             * allow its callback to execute inside delete_garbage(), it
             * could expose objects that have tp_clear already called on
             * them.  Or, it could resurrect unreachable objects.  One way
             * this can happen is if some container objects do not implement
             * tp_traverse.  Then, wr_object can be outside the unreachable
             * set but can be deallocated as a result of breaking the
             * reference cycle.  If we don't clear the weakref, the callback
             * will run and potentially cause a crash.  See bpo-38006 for
             * one example.
             */
            _PyWeakref_DetachRef((PyWeakReference *)op);
        }

        if (! _PyType_SUPPORTS_WEAKREFS(Py_TYPE(op)))
            continue;

        /* It supports weakrefs.  Does it have any?
         *
         * This is never triggered for static types so we can avoid the
         * (slightly) more costly _PyObject_GET_WEAKREFS_LISTPTR().
         */
        PyWeakrefBase *ctrl = (PyWeakrefBase *)_PyObject_GET_WEAKREF_CONTROL(op);

        if (!ctrl)
            continue;

        PyWeakrefBase *ref;
        for (ref = ctrl->wr_next; ref != ctrl; ref = ref->wr_next) {
            PyGC_Head *wrasgc;                  /* AS_GC(wr) */
            PyWeakReference *wr = (PyWeakReference *)ref;

            if (wr->wr_callback == NULL) {
                /* no callback */
                continue;
            }

            /* Headache time.  `op` is going away, and is weakly referenced by
             * `wr`, which has a callback.  Should the callback be invoked?  If wr
             * is also trash, no:
             *
             * 1. There's no need to call it.  The object and the weakref are
             *    both going away, so it's legitimate to pretend the weakref is
             *    going away first.  The user has to ensure a weakref outlives its
             *    referent if they want a guarantee that the wr callback will get
             *    invoked.
             *
             * 2. It may be catastrophic to call it.  If the callback is also in
             *    cyclic trash (CT), then although the CT is unreachable from
             *    outside the current generation, CT may be reachable from the
             *    callback.  Then the callback could resurrect insane objects.
             *
             * Since the callback is never needed and may be unsafe in this case,
             * wr is simply left in the unreachable set.  Note that because we
             * already called _PyWeakref_ClearRef(wr), its callback will never
             * trigger.
             *
             * OTOH, if wr isn't part of CT, we should invoke the callback:  the
             * weakref outlived the trash.  Note that since wr isn't CT in this
             * case, its callback can't be CT either -- wr acted as an external
             * root to this generation, and therefore its callback did too.  So
             * nothing in CT is reachable from the callback either, so it's hard
             * to imagine how calling it later could create a problem for us.  wr
             * is moved to wrcb_to_call in this case.
             */
            if (gc_is_unreachable(AS_GC(wr))) {
                /* it should already have been cleared above */
                // assert(wr->wr_object == Py_None);
                continue;
            }

            /* Create a new reference so that wr can't go away
             * before we can process it again.
             */
            Py_INCREF(wr);

            /* Move wr to wrcb_to_call, for the next pass. */
            wrasgc = AS_GC(wr);
            assert(wrasgc != next); /* wrasgc is reachable, but
                                       next isn't, so they can't
                                       be the same */
            assert(_PyGCHead_NEXT(wrasgc) == NULL);
            assert(_PyGCHead_PREV(wrasgc) == NULL);

            gc_list_append(wrasgc, wrcb_to_call);
        }

        /* Clear the root weakref but does not invoke any callbacks.
         * Other weak references reference this object
         */
        _PyObject_ClearWeakRefsFromGC(op);
    }
}

static int
call_weakref_callbacks(PyGC_Head *wrcb_to_call)
{
    PyGC_Head *gc;
    PyObject *op;               /* generally FROM_GC(gc) */
    int num_freed = 0;

    /* Invoke the callbacks we decided to honor.  It's safe to invoke them
     * because they can't reference unreachable objects.
     */
    while (! gc_list_is_empty(wrcb_to_call)) {
        PyObject *temp;
        PyObject *callback;

        gc = (PyGC_Head*)wrcb_to_call->_gc_next;
        gc_list_remove(gc);
        op = FROM_GC(gc);
        _PyObject_ASSERT(op, PyWeakref_Check(op));
        PyWeakReference *wr = (PyWeakReference *)op;
        callback = wr->wr_callback;
        _PyObject_ASSERT(op, callback != NULL);

        /* copy-paste of weakrefobject.c's handle_callback() */
        temp = PyObject_CallOneArg(callback, (PyObject *)wr);
        if (temp == NULL)
            PyErr_WriteUnraisable(callback);
        else
            Py_DECREF(temp);

        /* Give up the reference we created in the first pass.  When
         * op's refcount hits 0 (which it may or may not do right now),
         * op's tp_dealloc will decref op->wr_callback too.  Note
         * that the refcount probably will hit 0 now, and because this
         * weakref was reachable to begin with, gc didn't already
         * add it to its count of freed objects.  Example:  a reachable
         * weak value dict maps some key to this reachable weakref.
         * The callback removes this key->weakref mapping from the
         * dict, leaving no other references to the weakref (excepting
         * ours).
         */
        Py_DECREF(op);
    }

    return num_freed;
}

static void
debug_cycle(const char *msg, PyObject *op)
{
    PySys_FormatStderr("gc: %s <%s %p>\n",
                       msg, Py_TYPE(op)->tp_name, op);
}

/* Handle uncollectable garbage (cycles with tp_del slots, and stuff reachable
 * only from such cycles).
 * If DEBUG_SAVEALL, all objects in finalizers are appended to the module
 * garbage list (a Python list), else only the objects in finalizers with
 * __del__ methods are appended to garbage.  All objects in finalizers are
 * merged into the old list regardless.
 */
static void
handle_legacy_finalizers(PyThreadState *tstate,
                         GCState *gcstate,
                         PyGC_Head *finalizers)
{
    assert(!_PyErr_Occurred(tstate));
    assert(gcstate->garbage != NULL);

    PyGC_Head *gc = GC_NEXT(finalizers);
    for (; gc != finalizers; gc = GC_NEXT(gc)) {
        PyObject *op = FROM_GC(gc);

        if ((gcstate->debug & DEBUG_SAVEALL) || has_legacy_finalizer(op)) {
            if (PyList_Append(gcstate->garbage, op) < 0) {
                _PyErr_Clear(tstate);
                break;
            }
        }
    }

    gc_list_clear(finalizers);
}

static void
merge_queued_objects(_PyObjectQueue **to_dealloc_ptr)
{
    HEAD_LOCK(&_PyRuntime);
    PyThreadState *t;
    for_each_thread(t) {
        _Py_queue_process_gc(t, to_dealloc_ptr);
    }
    HEAD_UNLOCK(&_PyRuntime);
}

static void
dealloc_non_gc(_PyObjectQueue **queue_ptr)
{
    for (;;) {
        PyObject *op = _PyObjectQueue_Pop(queue_ptr);
        if (op == NULL) {
            break;
        }

        _Py_Dealloc(op);
    }
    assert(*queue_ptr == NULL);
}

static void
free_dict_keys(_PyObjectQueue **queue_ptr)
{
    for (;;) {
        PyDictSharedKeysObject *keys = (PyDictSharedKeysObject *)_PyObjectQueue_Pop(queue_ptr);
        if (keys == NULL) {
            break;
        }
        PyMem_Free(keys);
    }
    assert(*queue_ptr == NULL);
}

/* Run first-time finalizers (if any) on all the objects in collectable.
 * Note that this may remove some (or even all) of the objects from the
 * list, due to refcounts falling to 0.
 */
static void
finalize_garbage(PyThreadState *tstate, PyGC_Head *collectable)
{
    PyGC_Head *gc;
    PyObject *op;
    destructor finalize;

    /* While we're going through the loop, `finalize(op)` may cause op, or
     * other objects, to be reclaimed via refcounts falling to zero.  So
     * there's little we can rely on about the structure of the input
     * `collectable` list across iterations.  For safety, we always take the
     * first object in that list and move it to a temporary `seen` list.
     * If objects vanish from the `collectable` and `seen` lists we don't
     * care.
     */
    for (gc = GC_NEXT(collectable); gc != collectable; gc = GC_NEXT(gc)) {
        op = FROM_GC(gc);
        if (!_PyGCHead_FINALIZED(gc) &&
                (finalize = Py_TYPE(op)->tp_finalize) != NULL) {
            _PyGCHead_SET_FINALIZED(gc);
            finalize(op);
            assert(!_PyErr_Occurred(tstate));
        }
    }
}

/* Break reference cycles by clearing the containers involved.  This is
 * tricky business as the lists can be changing and we don't know which
 * objects may be freed.  It is possible I screwed something up here.
 */
static void
delete_garbage(PyThreadState *tstate, GCState *gcstate,
               PyGC_Head *collectable)
{
    assert(!_PyErr_Occurred(tstate));

    while (!gc_list_is_empty(collectable)) {
        PyGC_Head *gc = GC_NEXT(collectable);
        PyObject *op = FROM_GC(gc);

        gc_list_remove(gc);

        _PyObject_ASSERT_WITH_MSG(op, _Py_GC_REFCNT(op) > 0,
                                  "refcount is too small");

        if (gcstate->debug & DEBUG_SAVEALL) {
            assert(gcstate->garbage != NULL);
            if (PyList_Append(gcstate->garbage, op) < 0) {
                _PyErr_Clear(tstate);
            }
        }
        else {
            inquiry clear;
            if ((clear = Py_TYPE(op)->tp_clear) != NULL) {
                // printf("clearing %p (op=%p)\n", gc, op);
                (void) clear(op);
                if (_PyErr_Occurred(tstate)) {
                    _PyErr_WriteUnraisableMsg("in tp_clear of",
                                              (PyObject*)Py_TYPE(op));
                }
                // printf("refcnt after clear of %p = %d\n", gc, (int)_Py_GC_REFCNT(op));
            }
        }
        Py_DECREF(op);
    }
}

static void
clear_freelists(PyThreadState *tstate)
{
    _PyTuple_ClearFreeList(tstate);
    _PyFloat_ClearFreeList(tstate);
    _PyList_ClearFreeList(tstate);
    _PyDict_ClearFreeList(tstate);
    _PyAsyncGen_ClearFreeLists(tstate);
    _PyContext_ClearFreeList(tstate);
}

/* Clear all free lists
 * All free lists are cleared during the collection of the highest generation.
 * Allocated items in the free list may keep a pymalloc arena occupied.
 * Clearing the free lists may give back memory to the OS earlier.
 */
static void
clear_all_freelists(PyInterpreterState *interp)
{
    HEAD_LOCK(&_PyRuntime);
    PyThreadState *tstate = interp->threads.head;
    while (tstate != NULL) {
        clear_freelists(tstate);
        tstate = tstate->next;
    }
    HEAD_UNLOCK(&_PyRuntime);
}

/* Deduce which objects among "base" are unreachable from outside the list
   and move them to 'unreachable'. The process consist in the following steps:

1. Copy all reference counts to a different field (gc_prev is used to hold
   this copy to save memory).
2. Traverse all objects in "base" and visit all referred objects using
   "tp_traverse" and for every visited object, subtract 1 to the reference
   count (the one that we copied in the previous step). After this step, all
   objects that can be reached directly from outside must have strictly positive
   reference count, while all unreachable objects must have a count of exactly 0.
3. Identify all unreachable objects (the ones with 0 reference count) and move
   them to the "unreachable" list. This step also needs to move back to "base" all
   objects that were initially marked as unreachable but are referred transitively
   by the reachable objects (the ones with strictly positive reference count).

Contracts:

    * The "base" has to be a valid list with no mask set.

    * The "unreachable" list must be uninitialized (this function calls
      gc_list_init over 'unreachable').

IMPORTANT: This function leaves 'unreachable' with the NEXT_MASK_UNREACHABLE
flag set but it does not clear it to skip unnecessary iteration. Before the
flag is cleared (for example, by using 'clear_unreachable_mask' function or
by a call to 'move_legacy_finalizers'), the 'unreachable' list is not a normal
list and we can not use most gc_list_* functions for it. */
static inline void
deduce_unreachable(PyGC_Head *base, PyGC_Head *unreachable) {
    /* Leave everything reachable from outside base in base, and move
     * everything else (in base) to unreachable.
     *
     * NOTE:  This used to move the reachable objects into a reachable
     * set instead.  But most things usually turn out to be reachable,
     * so it's more efficient to move the unreachable things.  It "sounds slick"
     * to move the unreachable objects, until you think about it - the reason it
     * pays isn't actually obvious.
     *
     * Suppose we create objects A, B, C in that order.  They appear in the young
     * generation in the same order.  If B points to A, and C to B, and C is
     * reachable from outside, then the adjusted refcounts will be 0, 0, and 1
     * respectively.
     *
     * When move_unreachable finds A, A is moved to the unreachable list.  The
     * same for B when it's first encountered.  Then C is traversed, B is moved
     * _back_ to the reachable list.  B is eventually traversed, and then A is
     * moved back to the reachable list.
     *
     * So instead of not moving at all, the reachable objects B and A are moved
     * twice each.  Why is this a win?  A straightforward algorithm to move the
     * reachable objects instead would move A, B, and C once each.
     *
     * The key is that this dance leaves the objects in order C, B, A - it's
     * reversed from the original order.  On all _subsequent_ scans, none of
     * them will move.  Since most objects aren't in cycles, this can save an
     * unbounded number of moves across an unbounded number of later collections.
     * It can cost more only the first time the chain is scanned.
     *
     * Drawback:  move_unreachable is also used to find out what's still trash
     * after finalizers may resurrect objects.  In _that_ case most unreachable
     * objects will remain unreachable, so it would be more efficient to move
     * the reachable objects instead.  But this is a one-time cost, probably not
     * worth complicating the code to speed just a little.
     */
    gc_list_init(unreachable);
    move_unreachable(base, unreachable);  // gc_prev is pointer again
    validate_list(base, unreachable_clear);
    validate_list(unreachable, unreachable_set);
}

/* Handle objects that may have resurrected after a call to 'finalize_garbage', moving
   them to 'old_generation' and placing the rest on 'still_unreachable'.

   Contracts:
       * After this function 'unreachable' must not be used anymore and 'still_unreachable'
         will contain the objects that did not resurrect.

       * The "still_unreachable" list must be uninitialized (this function calls
         gc_list_init over 'still_unreachable').

IMPORTANT: After a call to this function, the 'still_unreachable' set will have the
PREV_MARK_COLLECTING set, but the objects in this set are going to be removed so
we can skip the expense of clearing the flag to avoid extra iteration. */
static inline void
handle_resurrected_objects(PyGC_Head *unreachable, PyGC_Head* still_unreachable)
{
    validate_list(unreachable, unreachable_set);

    // First reset the reference count for unreachable objects. Subtract one
    // from the reference count to account for the refcount increment due
    // to being in the "unreachable" list.
    PyGC_Head *gc;
    for (gc = GC_NEXT(unreachable); gc != unreachable; gc = GC_NEXT(gc)) {
        Py_ssize_t refcnt = _Py_GC_REFCNT(FROM_GC(gc));
        gc_set_refs(gc, refcnt - 1);
        _PyObject_ASSERT(FROM_GC(gc), refcnt > 0);
    }

    subtract_refs_unreachable(unreachable);
    clear_unreachable_mask(unreachable);

    // After the call to deduce_unreachable, the 'still_unreachable' set will
    // have the PREV_MARK_COLLECTING set, but the objects are going to be
    // removed so we can skip the expense of clearing the flag.
    PyGC_Head* resurrected = unreachable;
    deduce_unreachable(resurrected, still_unreachable);

    while (!gc_list_is_empty(resurrected)) {
        PyGC_Head *gc = GC_NEXT(resurrected);
        PyObject *op = FROM_GC(gc);

        gc_list_remove(gc);
        decref_merged(op);
    }
}

static void
update_gc_threshold(GCState *gcstate)
{
    Py_ssize_t live = _Py_atomic_load_ssize(&gcstate->gc_live);
    Py_ssize_t threshold = live + (live * gcstate->gc_scale) / 100;
    if (threshold < 7000) {
        threshold = 7000;
    }
    _Py_atomic_store_ssize(&gcstate->gc_threshold, threshold);
}

static int
gc_reason_is_valid(GCState *gcstate, _PyGC_Reason reason)
{
    if (reason == GC_REASON_HEAP) {
        return _PyGC_ShouldCollect(gcstate);
    }
    return 1;
}

static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   Py_ssize_t collected, Py_ssize_t uncollectable);

/* This is the main function.  Read this to understand how the
 * collection process works. */
static Py_ssize_t
gc_collect_main(PyThreadState *tstate, int generation, _PyGC_Reason reason)
{
    Py_ssize_t m = 0; /* # objects collected */
    Py_ssize_t n = 0; /* # unreachable objects that couldn't be collected */
    PyGC_Head young; /* the generation we are examining */
    PyGC_Head unreachable; /* non-problematic unreachable trash */
    PyGC_Head wrcb_to_call; /* weakrefs with callbacks to call */
    PyGC_Head finalizers;  /* objects with, & reachable from, __del__ */
    PyGC_Head *gc;
    _PyObjectQueue *to_dealloc = NULL;
    _PyTime_t t1 = 0;   /* initialize to prevent a compiler warning */
    GCState *gcstate = &tstate->interp->gc;

    // gc_collect_main() must not be called before _PyGC_Init
    // or after _PyGC_Fini()
    assert(gcstate->garbage != NULL);
    assert(!_PyErr_Occurred(tstate));

    if (tstate->cant_stop_wont_stop) {
        // Don't start a garbage collection if this thread is in a critical
        // section that doesn't allow GC.
        return 0;
    }

    if (!_Py_atomic_compare_exchange_int(&_PyRuntime.gc_collecting, 0, 1)) {
        // Don't start a garbage collection if a collection is already in
        // progress.
        return 0;
    }

    if (!gc_reason_is_valid(gcstate, reason)) {
        _Py_atomic_store_int(&_PyRuntime.gc_collecting, 0);
        return 0;
    }

    _Py_atomic_store_int(&gcstate->collecting, 1);

    _PyRuntimeState_StopTheWorld(&_PyRuntime);

    if (reason != GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "start", 0, 0);
    }

    if (gcstate->debug & DEBUG_STATS) {
        PySys_WriteStderr("gc: collecting heap...\n");
        PySys_FormatStderr(
            "gc: live objects: %"PY_FORMAT_SIZE_T"d\n",
            gcstate->gc_live);
        t1 = _PyTime_GetMonotonicClock();
    }

    if (PyDTrace_GC_START_ENABLED())
        PyDTrace_GC_START(NUM_GENERATIONS - 1);

    /* Merge the refcount for all queued objects, but do not dealloc
     * yet. Objects with zero refcount that are tracked will be freed during
     * GC. Non-tracked objects are added to "to_dealloc" and freed once
     * threads are resumed.
     */
    merge_queued_objects(&to_dealloc);
    validate_tracked_heap(_PyGC_PREV_MASK|_PyGC_PREV_MASK_UNREACHABLE, 0);

    gc_list_init(&young);
    struct update_refs_args args = { .list = &young, .split_keys_marked = 0 };
    visit_heaps2(mi_heap_tag_gc, update_refs, &args);

    _PyObjectQueue *dead_keys = NULL;
    int split_keys_unmarked = 0;
    find_dead_shared_keys(&dead_keys, &split_keys_unmarked);
    free_dict_keys(&dead_keys);
    assert(args.split_keys_marked == split_keys_unmarked);
    deduce_unreachable(&young, &unreachable);

    gcstate->long_lived_pending = 0;
    gcstate->long_lived_total = gc_list_size(&young);
    gc_list_clear(&young);

    /* All objects in unreachable are trash, but objects reachable from
     * legacy finalizers (e.g. tp_del) can't safely be deleted.
     */
    gc_list_init(&finalizers);
    // After move_legacy_finalizers(), unreachable is normal list.
    move_legacy_finalizers(&unreachable, &finalizers);
    /* finalizers contains the unreachable objects with a legacy finalizer;
     * unreachable objects reachable *from* those are also uncollectable,
     * and we move those into the finalizers list too.
     */
    move_legacy_finalizer_reachable(&finalizers);

    validate_list(&finalizers, unreachable_clear);
    validate_list(&unreachable, unreachable_set);

    /* Print debugging information. */
    if (gcstate->debug & DEBUG_COLLECTABLE) {
        for (gc = GC_NEXT(&unreachable); gc != &unreachable; gc = GC_NEXT(gc)) {
            debug_cycle("collectable", FROM_GC(gc));
        }
    }

    /* Clear weakrefs and invoke callbacks as necessary. */
    gc_list_init(&wrcb_to_call);
    clear_weakrefs(&unreachable, &wrcb_to_call);

    validate_list(&unreachable, unreachable_set);

    /* Restart the world to call weakrefs and finalizers */
    _PyRuntimeState_StartTheWorld(&_PyRuntime);

    /* Dealloc objects with zero refcount that are not tracked by GC */
    dealloc_non_gc(&to_dealloc);

    m += call_weakref_callbacks(&wrcb_to_call);

    /* Call tp_finalize on objects which have one. */
    finalize_garbage(tstate, &unreachable);

    _PyRuntimeState_StopTheWorld(&_PyRuntime);

    validate_refcount();

    /* Handle any objects that may have resurrected after the call
     * to 'finalize_garbage' and continue the collection with the
     * objects that are still unreachable */
    PyGC_Head final_unreachable;
    handle_resurrected_objects(&unreachable, &final_unreachable);

    /* Clear free list only during the collection of the highest
     * generation */
    if (generation == NUM_GENERATIONS-1) {
        clear_all_freelists(tstate->interp);
    }

    _PyRuntimeState_StartTheWorld(&_PyRuntime);

    /* Call tp_clear on objects in the final_unreachable set.  This will cause
    * the reference cycles to be broken.  It may also cause some objects
    * in finalizers to be freed.
    */
    m += gc_list_size(&final_unreachable);
    delete_garbage(tstate, gcstate, &final_unreachable);

    if (reason == GC_REASON_MANUAL) {
        // Clear this thread's freelists again after deleting garbage
        // for more precise block accounting when calling gc.collect().
        clear_freelists(tstate);
    }

    /* Collect statistics on uncollectable objects found and print
     * debugging information. */
    for (gc = GC_NEXT(&finalizers); gc != &finalizers; gc = GC_NEXT(gc)) {
        n++;
        if (gcstate->debug & DEBUG_UNCOLLECTABLE)
            debug_cycle("uncollectable", FROM_GC(gc));
    }
    if (gcstate->debug & DEBUG_STATS) {
        double d = _PyTime_AsSecondsDouble(_PyTime_GetPerfCounter() - t1);
        PySys_WriteStderr(
            "gc: done, %zd unreachable, %zd uncollectable, %.4fs elapsed\n",
            n+m, n, d);
    }

    /* Append instances in the uncollectable set to a Python
     * reachable list of garbage.  The programmer has to deal with
     * this if they insist on creating this type of structure.
     */
    handle_legacy_finalizers(tstate, gcstate, &finalizers);

    if (_PyErr_Occurred(tstate)) {
        if (reason == GC_REASON_SHUTDOWN) {
            _PyErr_Clear(tstate);
        }
        else {
            _PyErr_WriteUnraisableMsg("in garbage collection", NULL);
        }
    }

    /* Update stats */
    struct gc_generation_stats *stats = &gcstate->stats;
    stats->collections++;
    stats->collected += m;
    stats->uncollectable += n;

    update_gc_threshold(gcstate);

    if (PyDTrace_GC_DONE_ENABLED()) {
        PyDTrace_GC_DONE(n + m);
    }

    assert(!_PyErr_Occurred(tstate));

    if (reason != GC_REASON_SHUTDOWN) {
        invoke_gc_callback(tstate, "stop", m, n);
    }

    _Py_atomic_store_int(&gcstate->collecting, 0);
    _Py_atomic_store_int(&_PyRuntime.gc_collecting, 0);
    return n + m;
}

/* Invoke progress callbacks to notify clients that garbage collection
 * is starting or stopping
 */
static void
invoke_gc_callback(PyThreadState *tstate, const char *phase,
                   Py_ssize_t collected, Py_ssize_t uncollectable)
{
    assert(!_PyErr_Occurred(tstate));

    /* we may get called very early */
    GCState *gcstate = &tstate->interp->gc;
    if (gcstate->callbacks == NULL) {
        return;
    }

    /* The local variable cannot be rebound, check it for sanity */
    assert(PyList_CheckExact(gcstate->callbacks));
    PyObject *info = NULL;
    if (PyList_GET_SIZE(gcstate->callbacks) != 0) {
        info = Py_BuildValue("{sisnsn}",
            "generation", 0,    // what value maximizes compatiblity?
            "collected", collected,
            "uncollectable", uncollectable);
        if (info == NULL) {
            PyErr_WriteUnraisable(NULL);
            return;
        }
    }
    for (Py_ssize_t i=0; i<PyList_GET_SIZE(gcstate->callbacks); i++) {
        PyObject *r, *cb = PyList_GET_ITEM(gcstate->callbacks, i);
        Py_INCREF(cb); /* make sure cb doesn't go away */
        r = PyObject_CallFunction(cb, "sO", phase, info);
        if (r == NULL) {
            PyErr_WriteUnraisable(cb);
        }
        else {
            Py_DECREF(r);
        }
        Py_DECREF(cb);
    }
    Py_XDECREF(info);
    assert(!_PyErr_Occurred(tstate));
}

#include "clinic/gcmodule.c.h"

/*[clinic input]
gc.enable

Enable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_enable_impl(PyObject *module)
/*[clinic end generated code: output=45a427e9dce9155c input=81ac4940ca579707]*/
{
    PyGC_Enable();
    Py_RETURN_NONE;
}

/*[clinic input]
gc.disable

Disable automatic garbage collection.
[clinic start generated code]*/

static PyObject *
gc_disable_impl(PyObject *module)
/*[clinic end generated code: output=97d1030f7aa9d279 input=8c2e5a14e800d83b]*/
{
    PyGC_Disable();
    Py_RETURN_NONE;
}

/*[clinic input]
gc.isenabled -> bool

Returns true if automatic garbage collection is enabled.
[clinic start generated code]*/

static int
gc_isenabled_impl(PyObject *module)
/*[clinic end generated code: output=1874298331c49130 input=30005e0422373b31]*/
{
    return PyGC_IsEnabled();
}

/*[clinic input]
gc.collect -> Py_ssize_t

    generation: int(c_default="NUM_GENERATIONS - 1") = 2

Run the garbage collector.

With no arguments, run a full collection.  The optional argument
may be an integer specifying which generation to collect.  A ValueError
is raised if the generation number is invalid.

The number of unreachable objects is returned.
[clinic start generated code]*/

static Py_ssize_t
gc_collect_impl(PyObject *module, int generation)
/*[clinic end generated code: output=b697e633043233c7 input=40720128b682d879]*/
{
    PyThreadState *tstate = _PyThreadState_GET();

    if (generation < 0 || generation >= 3) {
        _PyErr_SetString(tstate, PyExc_ValueError, "invalid generation");
        return -1;
    }

    return gc_collect_main(tstate, generation, GC_REASON_MANUAL);
}

/*[clinic input]
gc.set_debug

    flags: int
        An integer that can have the following bits turned on:
          DEBUG_STATS - Print statistics during collection.
          DEBUG_COLLECTABLE - Print collectable objects found.
          DEBUG_UNCOLLECTABLE - Print unreachable but uncollectable objects
            found.
          DEBUG_SAVEALL - Save objects to gc.garbage rather than freeing them.
          DEBUG_LEAK - Debug leaking programs (everything but STATS).
    /

Set the garbage collection debugging flags.

Debugging information is written to sys.stderr.
[clinic start generated code]*/

static PyObject *
gc_set_debug_impl(PyObject *module, int flags)
/*[clinic end generated code: output=7c8366575486b228 input=5e5ce15e84fbed15]*/
{
    GCState *gcstate = get_gc_state();
    gcstate->debug = flags;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_debug -> int

Get the garbage collection debugging flags.
[clinic start generated code]*/

static int
gc_get_debug_impl(PyObject *module)
/*[clinic end generated code: output=91242f3506cd1e50 input=91a101e1c3b98366]*/
{
    GCState *gcstate = get_gc_state();
    return gcstate->debug;
}

PyDoc_STRVAR(gc_set_thresh__doc__,
"set_threshold(threshold0, [threshold1, threshold2]) -> None\n"
"\n"
"Sets the collection thresholds.  Setting threshold0 to zero disables\n"
"collection.\n");

static PyObject *
gc_set_threshold(PyObject *self, PyObject *args)
{
    GCState *gcstate = get_gc_state();
    int threshold0, threshold1, threshold2;

    if (!PyArg_ParseTuple(args, "i|ii:set_threshold",
                          &threshold0,
                          &threshold1,
                          &threshold2))
        return NULL;

    // FIXME: does setting threshold0 to zero actually disable collection ???
    gcstate->gc_threshold = threshold0;
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_threshold

Return the current collection thresholds.
[clinic start generated code]*/

static PyObject *
gc_get_threshold_impl(PyObject *module)
/*[clinic end generated code: output=7902bc9f41ecbbd8 input=286d79918034d6e6]*/
{
    GCState *gcstate = get_gc_state();
    return Py_BuildValue("(nii)",
                         gcstate->gc_threshold,
                         0,
                         0);
}

/*[clinic input]
gc.get_count

Return a three-tuple of the current collection counts.
[clinic start generated code]*/

static PyObject *
gc_get_count_impl(PyObject *module)
/*[clinic end generated code: output=354012e67b16398f input=a392794a08251751]*/
{
    GCState *gcstate = get_gc_state();
    Py_ssize_t gc_live = _Py_atomic_load_ssize(&gcstate->gc_live);
    return Py_BuildValue("(nii)", gc_live, 0, 0);
}

static int
referrersvisit(PyObject* obj, PyObject *objs)
{
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(objs); i++)
        if (PyTuple_GET_ITEM(objs, i) == obj)
            return 1;
    return 0;
}

struct gc_referrers_arg {
    PyObject *objs;
    PyObject *resultlist;
};

static int
gc_referrers_visitor(PyGC_Head *gc, void *void_arg)
{
    struct gc_referrers_arg *arg = (struct gc_referrers_arg*)void_arg;
    PyObject *objs = arg->objs;
    PyObject *resultlist = arg->resultlist;

    PyObject *obj = FROM_GC(gc);
    traverseproc traverse = Py_TYPE(obj)->tp_traverse;
    if (obj == objs || obj == resultlist) {
        return 0;
    }
    if (traverse(obj, (visitproc)referrersvisit, objs)) {
        if (PyList_Append(resultlist, obj) < 0) {
            return -1; /* error */
        }
    }
    return 0;
}

PyDoc_STRVAR(gc_get_referrers__doc__,
"get_referrers(*objs) -> list\n\
Return the list of objects that directly refer to any of objs.");

static PyObject *
gc_get_referrers(PyObject *self, PyObject *args)
{
    if (PySys_Audit("gc.get_referrers", "(O)", args) < 0) {
        return NULL;
    }

    PyObject *result = PyList_New(0);
    if (!result) {
        return NULL;
    }

    struct gc_referrers_arg arg;
    arg.objs = args;
    arg.resultlist = result;
    if (visit_heaps(gc_referrers_visitor, &arg) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

/* Append obj to list; return true if error (out of memory), false if OK. */
static int
referentsvisit(PyObject *obj, PyObject *list)
{
    return PyList_Append(list, obj) < 0;
}

PyDoc_STRVAR(gc_get_referents__doc__,
"get_referents(*objs) -> list\n\
Return the list of objects that are directly referred to by objs.");

static PyObject *
gc_get_referents(PyObject *self, PyObject *args)
{
    Py_ssize_t i;
    if (PySys_Audit("gc.get_referents", "(O)", args) < 0) {
        return NULL;
    }
    PyObject *result = PyList_New(0);

    if (result == NULL)
        return NULL;

    for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
        traverseproc traverse;
        PyObject *obj = PyTuple_GET_ITEM(args, i);

        if (!_PyObject_IS_GC(obj))
            continue;
        traverse = Py_TYPE(obj)->tp_traverse;
        if (! traverse)
            continue;
        if (traverse(obj, (visitproc)referentsvisit, result)) {
            Py_DECREF(result);
            return NULL;
        }
    }
    return result;
}

struct gc_get_objects_arg {
    PyObject *py_list;
    Py_ssize_t generation;
};

static int
gc_get_objects_visitor(PyGC_Head *gc, void *void_arg)
{
    PyObject *op = FROM_GC(gc);

    struct gc_get_objects_arg *arg = (struct gc_get_objects_arg*)void_arg;
    PyObject *py_list = arg->py_list;

    if (op == py_list) {
        return 0;
    }
    if (PyList_Append(py_list, op)) {
        return -1;
    }
    return 0;
}

/*[clinic input]
gc.get_objects
    generation: Py_ssize_t(accept={int, NoneType}, c_default="-1") = None
        Generation to extract the objects from.

Return a list of objects tracked by the collector (excluding the list returned).

If generation is not None, return only the objects tracked by the collector
that are in that generation.
[clinic start generated code]*/

static PyObject *
gc_get_objects_impl(PyObject *module, Py_ssize_t generation)
/*[clinic end generated code: output=48b35fea4ba6cb0e input=ef7da9df9806754c]*/
{
    PyObject* result;

    if (PySys_Audit("gc.get_objects", "n", generation) < 0) {
        return NULL;
    }

    result = PyList_New(0);
    if (result == NULL) {
        return NULL;
    }

    if (generation >= NUM_GENERATIONS) {
        PyErr_Format(PyExc_ValueError,
                    "generation parameter must be less than the number of "
                    "available generations (%i)",
                    NUM_GENERATIONS);
        goto error;
    }

    /* If generation is passed, we extract only that generation */
    if (generation < -1) {
        PyErr_SetString(PyExc_ValueError,
                        "generation parameter cannot be negative");
        goto error;
    }

    struct gc_get_objects_arg arg;
    arg.py_list = result;
    arg.generation = generation + 1;
    if (visit_heaps(gc_get_objects_visitor, &arg) < 0) {
        goto error;
    }

    return result;

error:
    Py_DECREF(result);
    return NULL;
}

/*[clinic input]
gc.get_stats

Return a list of dictionaries containing per-generation statistics.
[clinic start generated code]*/

static PyObject *
gc_get_stats_impl(PyObject *module)
/*[clinic end generated code: output=a8ab1d8a5d26f3ab input=1ef4ed9d17b1a470]*/
{
    struct gc_generation_stats stats;
    PyObject *result, *dict;

    /* To get consistent values despite allocations while constructing
       the result list, we use a snapshot of the running stats. */
    stats = get_gc_state()->stats;

    result = PyList_New(0);
    if (result == NULL)
        return NULL;

    dict = Py_BuildValue("{snsnsn}",
                         "collections", stats.collections,
                         "collected", stats.collected,
                         "uncollectable", stats.uncollectable
                        );
    if (dict == NULL)
        goto error;
    if (PyList_Append(result, dict)) {
        Py_DECREF(dict);
        goto error;
    }
    Py_DECREF(dict);
    return result;

error:
    Py_XDECREF(result);
    return NULL;
}


/*[clinic input]
gc.is_tracked

    obj: object
    /

Returns true if the object is tracked by the garbage collector.

Simple atomic objects will return false.
[clinic start generated code]*/

static PyObject *
gc_is_tracked(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=14f0103423b28e31 input=d83057f170ea2723]*/
{
    PyObject *result;

    if (_PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj))
        result = Py_True;
    else
        result = Py_False;
    return Py_NewRef(result);
}

/*[clinic input]
gc.is_finalized

    obj: object
    /

Returns true if the object has been already finalized by the GC.
[clinic start generated code]*/

static PyObject *
gc_is_finalized(PyObject *module, PyObject *obj)
/*[clinic end generated code: output=e1516ac119a918ed input=201d0c58f69ae390]*/
{
    if (_PyObject_IS_GC(obj) && _PyGCHead_FINALIZED(AS_GC(obj))) {
         Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

/*[clinic input]
gc.freeze

Freeze all current tracked objects and ignore them for future collections.

This can be used before a POSIX fork() call to make the gc copy-on-write friendly.
Note: collection before a POSIX fork() call may free pages for future allocation
which can cause copy-on-write.
[clinic start generated code]*/

static PyObject *
gc_freeze_impl(PyObject *module)
/*[clinic end generated code: output=502159d9cdc4c139 input=b602b16ac5febbe5]*/
{
    // we only have a single generation, so this doesn't do anything
    // TODO: untrack objects?
    Py_RETURN_NONE;
}

/*[clinic input]
gc.unfreeze

Unfreeze all objects in the permanent generation.

Put all objects in the permanent generation back into oldest generation.
[clinic start generated code]*/

static PyObject *
gc_unfreeze_impl(PyObject *module)
/*[clinic end generated code: output=1c15f2043b25e169 input=2dd52b170f4cef6c]*/
{
    // we only have a single generation, so this doesn't do anything
    Py_RETURN_NONE;
}

/*[clinic input]
gc.get_freeze_count -> Py_ssize_t

Return the number of objects in the permanent generation.
[clinic start generated code]*/

static Py_ssize_t
gc_get_freeze_count_impl(PyObject *module)
/*[clinic end generated code: output=61cbd9f43aa032e1 input=45ffbc65cfe2a6ed]*/
{
    return 0;
}


PyDoc_STRVAR(gc__doc__,
"This module provides access to the garbage collector for reference cycles.\n"
"\n"
"enable() -- Enable automatic garbage collection.\n"
"disable() -- Disable automatic garbage collection.\n"
"isenabled() -- Returns true if automatic collection is enabled.\n"
"collect() -- Do a full collection right now.\n"
"get_count() -- Return the current collection counts.\n"
"get_stats() -- Return list of dictionaries containing per-generation stats.\n"
"set_debug() -- Set debugging flags.\n"
"get_debug() -- Get debugging flags.\n"
"set_threshold() -- Set the collection thresholds.\n"
"get_threshold() -- Return the current the collection thresholds.\n"
"get_objects() -- Return a list of all objects tracked by the collector.\n"
"is_tracked() -- Returns true if a given object is tracked.\n"
"is_finalized() -- Returns true if a given object has been already finalized.\n"
"get_referrers() -- Return the list of objects that refer to an object.\n"
"get_referents() -- Return the list of objects that an object refers to.\n"
"freeze() -- Freeze all tracked objects and ignore them for future collections.\n"
"unfreeze() -- Unfreeze all objects in the permanent generation.\n"
"get_freeze_count() -- Return the number of objects in the permanent generation.\n");

static PyMethodDef GcMethods[] = {
    GC_ENABLE_METHODDEF
    GC_DISABLE_METHODDEF
    GC_ISENABLED_METHODDEF
    GC_SET_DEBUG_METHODDEF
    GC_GET_DEBUG_METHODDEF
    GC_GET_COUNT_METHODDEF
    {"set_threshold",  gc_set_threshold, METH_VARARGS, gc_set_thresh__doc__},
    GC_GET_THRESHOLD_METHODDEF
    GC_COLLECT_METHODDEF
    GC_GET_OBJECTS_METHODDEF
    GC_GET_STATS_METHODDEF
    GC_IS_TRACKED_METHODDEF
    GC_IS_FINALIZED_METHODDEF
    {"get_referrers",  gc_get_referrers, METH_VARARGS,
        gc_get_referrers__doc__},
    {"get_referents",  gc_get_referents, METH_VARARGS,
        gc_get_referents__doc__},
    GC_FREEZE_METHODDEF
    GC_UNFREEZE_METHODDEF
    GC_GET_FREEZE_COUNT_METHODDEF
    {NULL,      NULL}           /* Sentinel */
};

static int
gcmodule_exec(PyObject *module)
{
    GCState *gcstate = get_gc_state();

    /* garbage and callbacks are initialized by _PyGC_Init() early in
     * interpreter lifecycle. */
    assert(gcstate->garbage != NULL);
    if (PyModule_AddObjectRef(module, "garbage", gcstate->garbage) < 0) {
        return -1;
    }
    assert(gcstate->callbacks != NULL);
    if (PyModule_AddObjectRef(module, "callbacks", gcstate->callbacks) < 0) {
        return -1;
    }

#define ADD_INT(NAME) if (PyModule_AddIntConstant(module, #NAME, NAME) < 0) { return -1; }
    ADD_INT(DEBUG_STATS);
    ADD_INT(DEBUG_COLLECTABLE);
    ADD_INT(DEBUG_UNCOLLECTABLE);
    ADD_INT(DEBUG_SAVEALL);
    ADD_INT(DEBUG_LEAK);
#undef ADD_INT
    return 0;
}

static PyModuleDef_Slot gcmodule_slots[] = {
    {Py_mod_exec, gcmodule_exec},
    {0, NULL}
};

static struct PyModuleDef gcmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "gc",
    .m_doc = gc__doc__,
    .m_size = 0,  // per interpreter state, see: get_gc_state()
    .m_methods = GcMethods,
    .m_slots = gcmodule_slots
};

PyMODINIT_FUNC
PyInit_gc(void)
{
    return PyModuleDef_Init(&gcmodule);
}

/* C API for controlling the state of the garbage collector */
int
PyGC_Enable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 1;
    return old_state;
}

int
PyGC_Disable(void)
{
    GCState *gcstate = get_gc_state();
    int old_state = gcstate->enabled;
    gcstate->enabled = 0;
    return old_state;
}

int
PyGC_IsEnabled(void)
{
    GCState *gcstate = get_gc_state();
    return gcstate->enabled;
}

/* Public API to invoke gc.collect() from C */
Py_ssize_t
PyGC_Collect(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    GCState *gcstate = &tstate->interp->gc;

    if (!gcstate->enabled) {
        return 0;
    }

    return gc_collect_main(tstate, NUM_GENERATIONS - 1, GC_REASON_MANUAL);
}

Py_ssize_t
_PyGC_CollectNoFail(PyThreadState *tstate)
{
    assert(!_PyErr_Occurred(tstate));
    /* Ideally, this function is only called on interpreter shutdown,
       and therefore not recursively.  Unfortunately, when there are daemon
       threads, a daemon thread can start a cyclic garbage collection
       during interpreter shutdown (and then never finish it).
       See http://bugs.python.org/issue8713#msg195178 for an example.
       */
    return gc_collect_main(tstate, NUM_GENERATIONS - 1, GC_REASON_SHUTDOWN);
}

void
_PyGC_DumpShutdownStats(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    if (!(gcstate->debug & DEBUG_SAVEALL)
        && gcstate->garbage != NULL && PyList_GET_SIZE(gcstate->garbage) > 0) {
        const char *message;
        if (gcstate->debug & DEBUG_UNCOLLECTABLE)
            message = "gc: %zd uncollectable objects at " \
                "shutdown";
        else
            message = "gc: %zd uncollectable objects at " \
                "shutdown; use gc.set_debug(gc.DEBUG_UNCOLLECTABLE) to list them";
        /* PyErr_WarnFormat does too many things and we are at shutdown,
           the warnings module's dependencies (e.g. linecache) may be gone
           already. */
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, message,
                                     PyList_GET_SIZE(gcstate->garbage)))
            PyErr_WriteUnraisable(NULL);
        if (gcstate->debug & DEBUG_UNCOLLECTABLE) {
            PyObject *repr = NULL, *bytes = NULL;
            repr = PyObject_Repr(gcstate->garbage);
            if (!repr || !(bytes = PyUnicode_EncodeFSDefault(repr)))
                PyErr_WriteUnraisable(gcstate->garbage);
            else {
                PySys_WriteStderr(
                    "      %s\n",
                    PyBytes_AS_STRING(bytes)
                    );
            }
            Py_XDECREF(repr);
            Py_XDECREF(bytes);
        }
    }
}

static void
gc_fini_untrack(GCState *gcstate)
{
    // PyGC_Head *gc;
    // for (gc = GC_NEXT(list); gc != list; gc = GC_NEXT(list)) {
    //     PyObject *op = FROM_GC(gc);
    //     _PyObject_GC_UNTRACK(op);
    //     // gh-92036: If a deallocator function expect the object to be tracked
    //     // by the GC (ex: func_dealloc()), it can crash if called on an object
    //     // which is no longer tracked by the GC. Leak one strong reference on
    //     // purpose so the object is never deleted and its deallocator is not
    //     // called.
    //     Py_INCREF(op);
    // }
}


void
_PyGC_Fini(PyInterpreterState *interp)
{
    GCState *gcstate = &interp->gc;
    Py_CLEAR(gcstate->garbage);
    Py_CLEAR(gcstate->callbacks);

    if (!_Py_IsMainInterpreter(interp)) {
        // bpo-46070: Explicitly untrack all objects currently tracked by the
        // GC. Otherwise, if an object is used later by another interpreter,
        // calling PyObject_GC_UnTrack() on the object crashs if the previous
        // or the next object of the PyGC_Head structure became a dangling
        // pointer.
        gc_fini_untrack(gcstate);
    }
}

/* for debugging */
void
_PyGC_Dump(PyGC_Head *g)
{
    _PyObject_Dump(FROM_GC(g));
}


#ifdef Py_DEBUG
static int
visit_validate(PyObject *op, void *parent_raw)
{
    PyObject *parent = _PyObject_CAST(parent_raw);
    if (_PyObject_IsFreed(op)) {
        _PyObject_ASSERT_FAILED_MSG(parent,
                                    "PyObject_GC_Track() object is not valid");
    }
    return 0;
}
#endif


/* extension modules might be compiled with GC support so these
   functions must always be available */

void
PyObject_GC_Track(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_ASSERT_FAILED_MSG(op,
                                    "object already tracked "
                                    "by the garbage collector");
    }
    _PyObject_GC_TRACK(op);

#ifdef Py_DEBUG
    /* Check that the object is valid: validate objects traversed
       by tp_traverse() */
    traverseproc traverse = Py_TYPE(op)->tp_traverse;
    (void)traverse(op, visit_validate, op);
#endif
}

void
PyObject_GC_UnTrack(void *op_raw)
{
    PyObject *op = _PyObject_CAST(op_raw);
    /* Obscure:  the Py_TRASHCAN mechanism requires that we be able to
     * call PyObject_GC_UnTrack twice on an object.
     */
    if (_PyObject_GC_IS_TRACKED(op)) {
        _PyObject_GC_UNTRACK(op);
    }
}

int
PyObject_IS_GC(PyObject *obj)
{
    return _PyObject_IS_GC(obj);
}

void
_Py_RunGC(PyThreadState *tstate)
{
    gc_collect_main(tstate, 0, GC_REASON_HEAP);
}

static PyObject *
gc_alloc(size_t basicsize, size_t presize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (basicsize > PY_SSIZE_T_MAX - presize) {
        return _PyErr_NoMemory(tstate);
    }
    size_t size = presize + basicsize;
    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    char *mem = a->malloc(a->ctx, size);
    if (mem == NULL) {
        return _PyErr_NoMemory(tstate);
    }
    memset(mem, 0, presize);
    return (PyObject *)(mem + presize);
}

PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    size_t presize = _PyType_PreHeaderSize(tp);
    PyObject *op = gc_alloc(_PyObject_SIZE(tp), presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_Init(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size_t presize = _PyType_PreHeaderSize(tp);
    size_t size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *)gc_alloc(size, presize);
    if (op == NULL) {
        return NULL;
    }
    _PyObject_InitVar(op, tp, nitems);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    size_t presize = _PyType_PreHeaderSize(Py_TYPE(op));
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > (size_t)PY_SSIZE_T_MAX - presize) {
        return (PyVarObject *)PyErr_NoMemory();
    }

    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    char *mem = (char *)op - presize;
    mem = a->realloc(a->ctx, mem, presize + basicsize);
    if (mem == NULL)
        return (PyVarObject *)PyErr_NoMemory();
    op = (PyVarObject *) (mem + presize);
    Py_SET_SIZE(op, nitems);
    return op;
}

void
PyObject_GC_Del(void *op)
{
    size_t presize = _PyType_PreHeaderSize(((PyObject *)op)->ob_type);
    PyGC_Head *g = AS_GC(op);
    if (_PyObject_GC_IS_TRACKED(op)) {
#ifdef Py_DEBUG
        if (PyErr_WarnExplicitFormat(PyExc_ResourceWarning, "gc", 0,
                                     "gc", NULL, "Object of type %s is not untracked before destruction",
                                     ((PyObject*)op)->ob_type->tp_name)) {
            PyErr_WriteUnraisable(NULL);
        }
#endif
        gc_list_remove(g);
    }
    PyMemAllocatorEx *a = &_PyRuntime.allocators.standard.gc;
    a->free(a->ctx, ((char *)op)-presize);
}

int
PyObject_GC_IsTracked(PyObject* obj)
{
    if (_PyObject_IS_GC(obj) && _PyObject_GC_IS_TRACKED(obj)) {
        return 1;
    }
    return 0;
}

int
PyObject_GC_IsFinalized(PyObject *obj)
{
    if (_PyObject_IS_GC(obj) && _PyGCHead_FINALIZED(AS_GC(obj))) {
         return 1;
    }
    return 0;
}
