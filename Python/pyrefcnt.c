/* Implementation of biased reference counting */

#include "Python.h"
#include "pycore_pystate.h"
#include "lock.h"

// TODO: not efficient

typedef struct {
    _PyMutex mutex;
    struct llist_node threads;
} Bucket;

struct brc_queued_object {
    PyObject *object;
    struct brc_queued_object* next;
};

#define NUM_BUCKETS 251

static Bucket buckets[NUM_BUCKETS];

static struct _PyBrcState *
find_brc_state(Bucket *bucket, uintptr_t thread_id)
{
    struct llist_node *node;
    llist_for_each(node, &bucket->threads) {
        struct _PyBrcState *brc = llist_data(node, struct _PyBrcState, node);
        if (brc->thread_id == thread_id) {
            return brc;
        }
    }
    return NULL;
}

void
_Py_queue_object(PyObject *ob, uintptr_t tid)
{
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    if (tid == 0) {
        if (_PyObject_IS_IMMORTAL(ob)) {
            // kind of awkward, but strings can be immortalized after they have
            // a bunch of references and the new interpreter still tries decrefing
            // the immortalized object.
            return;
        }
        Py_FatalError("_Py_queue_object called with unowned object");
    }

    _PyMutex_lock(&bucket->mutex);
    struct _PyBrcState *brc = find_brc_state(bucket, tid);
    if (!brc) {
        // If we didn't find the owning thread then it must have already exited.
        // It's safe (and necessary) to merge the refcount.
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob);
        _PyMutex_unlock(&bucket->mutex);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
        return;
    }

    struct brc_queued_object *entry = PyMem_RawMalloc(sizeof(struct brc_queued_object));
    if (!entry) {
        // TODO(sgross): we can probably catch these later on in garbage collection.
        _PyMutex_unlock(&bucket->mutex);
        fprintf(stderr, "error: unable to allocate entry for brc_queued_object\n");
        return;
    }
    entry->object = ob;
    entry->next = brc->queue;
    brc->queue = entry;

    // Notify owning thread
    PyThreadStateOS *os = llist_data(brc, PyThreadStateOS, brc);
    _PyThreadState_Signal(os->tstate, EVAL_EXPLICIT_MERGE);

    _PyMutex_unlock(&bucket->mutex);
}

static void
_Py_queue_merge_objects(struct brc_queued_object* head)
{
    struct brc_queued_object* next;
    while (head) {
        next = head->next;
        PyObject *ob = head->object;
        Py_ssize_t refcount = _Py_ExplicitMergeRefcount(ob);
        if (refcount == 0) {
            _Py_Dealloc(ob);
        }
        PyMem_RawFree(head);
        head = next;
    }
}

void
_Py_queue_process(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct _PyBrcState *brc = &tstate->os->brc;
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    struct brc_queued_object* head;

    _PyMutex_lock(&bucket->mutex);
    head = brc->queue;
    brc->queue = NULL;
    _PyMutex_unlock(&bucket->mutex);

    _Py_queue_merge_objects(head);
}

void
_Py_queue_create(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct _PyBrcState *brc = &tstate->os->brc;
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    brc->queue = NULL;
    brc->thread_id = tid;

    _PyMutex_lock(&bucket->mutex);
    if (bucket->threads.next == NULL) {
        llist_init(&bucket->threads);
    }
    llist_insert_tail(&bucket->threads, &brc->node);
    _PyMutex_unlock(&bucket->mutex);
}

void
_Py_queue_destroy(PyThreadState *tstate)
{
    uintptr_t tid = tstate->fast_thread_id;
    struct _PyBrcState *brc = &tstate->os->brc;
    Bucket *bucket = &buckets[tid % NUM_BUCKETS];

    struct brc_queued_object* queue;

    _PyMutex_lock(&bucket->mutex);
    if (brc->node.next) {
        llist_remove(&brc->node);
    }
    queue = brc->queue;
    brc->queue = NULL;
    _PyMutex_unlock(&bucket->mutex);

    if (queue) {
        _Py_queue_merge_objects(queue);
    }
}
