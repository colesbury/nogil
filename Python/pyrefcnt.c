/* Implementation of biased reference counting */

#include "Python.h"
#include "pycore_llist.h"
#include "pycore_pystate.h"
#include "pycore_refcnt.h"

// TODO: not efficient

typedef struct {
    _PyMutex mutex;
    struct llist_node threads;
} Bucket;

#define NUM_BUCKETS 251

static Bucket buckets[NUM_BUCKETS];

static inline struct brc_state *
brc_state(PyThreadState *tstate)
{
    return &((PyThreadStateImpl *)tstate)->brc;
}

static PyThreadStateImpl *
find_thread_state(Bucket *bucket, uintptr_t thread_id)
{
    struct llist_node *node;
    llist_for_each(node, &bucket->threads) {
        PyThreadStateImpl *ts;
        ts = llist_data(node, PyThreadStateImpl, brc.bucket_node);
        if (ts->tstate.fast_thread_id == thread_id) {
            return ts;
        }
    }
    return NULL;
}


_PyObjectQueue *
_PyObjectQueue_New(void)
{
    PyThreadStateImpl *tstate_impl = _PyThreadStateImpl_GET();
    if (tstate_impl && tstate_impl->cached_queue) {
        _PyObjectQueue *q = tstate_impl->cached_queue;
        tstate_impl->cached_queue = NULL;
        return q;
    }

    _PyObjectQueue *q = PyMem_RawMalloc(sizeof(_PyObjectQueue));
    if (q == NULL) {
        Py_FatalError("gc: failed to allocate object queue");
    }
    q->prev = NULL;
    q->n = 0;
    return q;
}

void
_PyObjectQueue_Free(_PyObjectQueue *q)
{
    PyThreadStateImpl *tstate_impl = _PyThreadStateImpl_GET();
    if (tstate_impl && tstate_impl->cached_queue == NULL) {
        tstate_impl->cached_queue = q;
    }
    else {
        PyMem_RawFree(q);
    }
}

void
_PyObjectQueue_ClearFreeList(PyThreadStateImpl *tstate_impl)
{
    _PyObjectQueue *q = tstate_impl->cached_queue;
    if (q != NULL) {
        tstate_impl->cached_queue = NULL;
        PyMem_RawFree(q);
    }
}

void
_PyObjectQueue_Merge(_PyObjectQueue **dst_ptr, _PyObjectQueue **src_ptr)
{
    _PyObjectQueue *dst = *dst_ptr;
    _PyObjectQueue *src = *src_ptr;
    if (src == NULL) {
        return;
    }
    if (dst == NULL || (dst->n == 0 && dst->prev == NULL)) {
        *dst_ptr = src;
        *src_ptr = dst;
        return;
    }

    _PyObjectQueue *last = src;
    while (last->prev != NULL) {
        last = last->prev;
    }
    last->prev = dst;
    *dst_ptr = src;
    *src_ptr = NULL;
}


void
_Py_queue_object(PyObject *ob, uintptr_t tid)
{
    PyThreadStateImpl *tstate_impl;
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];
    assert(tid != 0);

    _PyMutex_lock(&bucket->mutex);
    tstate_impl = find_thread_state(bucket, tid);
    if (!tstate_impl) {
        // If we didn't find the owning thread then it must have already exited.
        // It's safe (and necessary) to merge the refcount. Subtract one when
        // merging because we've stolen a reference.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        _PyMutex_unlock(&bucket->mutex);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
        return;
    }

    _PyObjectQueue_Push(&tstate_impl->brc.queue, ob);

    // Notify owning thread
    _PyThreadState_Signal(&tstate_impl->tstate, EVAL_EXPLICIT_MERGE);

    _PyMutex_unlock(&bucket->mutex);
}

static void
_Py_queue_merge_objects(struct brc_state *brc)
{
    // Process all objects to be merged in the local queue.
    // Note that _Py_Dealloc call can reentrantly call into
    // this function.
    for (;;) {
        PyObject *ob = _PyObjectQueue_Pop(&brc->local_queue);
        if (ob == NULL) {
            break;
        }

        // Subtract one when merging refcount because the queue
        // owned a reference.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
    }
}

void
_Py_queue_process(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct brc_state *brc = brc_state(tstate);
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    assert(brc->bucket_node.next != NULL);

    // Append all objects from "queue" into "local_queue"
    _PyMutex_lock(&bucket->mutex);
    _PyObjectQueue_Merge(&brc->local_queue, &brc->queue);
    _PyMutex_unlock(&bucket->mutex);

    // Process "local_queue" until it's empty
    _Py_queue_merge_objects(brc);
}

void
_Py_queue_process_gc(PyThreadState *tstate, _PyObjectQueue **queue_ptr)
{
    struct brc_state *brc = brc_state(tstate);

    if (brc->bucket_node.next == NULL) {
        // thread isn't finish initializing
        return;
    }

    _PyObjectQueue_Merge(&brc->local_queue, &brc->queue);

    for (;;) {
        PyObject *ob = _PyObjectQueue_Pop(&brc->local_queue);
        if (ob == NULL) {
            break;
        }

        // Subtract one when merging refcount because the queue
        // owned a reference.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob, -1);
        if (refcount == 0) {
            if (!PyObject_GC_IsTracked(ob)) {
                _PyObjectQueue_Push(queue_ptr, ob);
            }
        }
    }
}

void
_Py_queue_create(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct brc_state *brc = brc_state(tstate);
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    brc->queue = NULL;
    brc->local_queue = NULL;

    _PyMutex_lock(&bucket->mutex);
    if (bucket->threads.next == NULL) {
        llist_init(&bucket->threads);
    }
    llist_insert_tail(&bucket->threads, &brc->bucket_node);
    _PyMutex_unlock(&bucket->mutex);
}

void
_Py_queue_destroy(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct brc_state *brc = brc_state(tstate);
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    _PyMutex_lock(&bucket->mutex);
    if (brc->bucket_node.next) {
        llist_remove(&brc->bucket_node);
        _PyObjectQueue_Merge(&brc->local_queue, &brc->queue);
    }
    _PyMutex_unlock(&bucket->mutex);

    // Process "local_queue" until it's empty
    _Py_queue_merge_objects(brc);
}

void
_Py_queue_after_fork(void)
{
    // Unlock all bucket mutexes. Some of the buckets may be locked because
    // locks can be handed off to a parked thread (see lock.c). We don't have
    // to worry about consistency here, becuase no thread can be actively
    // modifying a bucket, but it might be paused (not yet woken up) on a
    // _PyMutex_lock while holding that lock.
    for (int i = 0; i < NUM_BUCKETS; i++) {
        memset(&buckets[i].mutex, 0, sizeof(buckets[i].mutex));
    }
}
