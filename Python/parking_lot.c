#include "Python.h"

#include "pycore_pystate.h"
#include "condvar.h"

#include "lock.h"
#include "parking_lot.h"
#include "pyatomic.h"

typedef struct {
    _PyRawMutex mutex;

    struct llist_node root;
    size_t num_waiters;
} Bucket;

#define NUM_BUCKETS 251

static Bucket buckets[NUM_BUCKETS];

Py_DECL_THREAD Waiter *this_waiter;

Waiter *
_PyParkingLot_InitThread(void)
{
    if (this_waiter != NULL) {
        this_waiter->refcount++;
        return this_waiter;
    }
    Waiter *waiter = PyMem_RawMalloc(sizeof(Waiter));
    if (waiter == NULL) {
        return NULL;
    }
    memset(waiter, 0, sizeof(Waiter));
    waiter->thread_id = _Py_ThreadId();
    waiter->refcount++;
    if (PyMUTEX_INIT(&waiter->mutex)) {
        PyMem_RawFree(waiter);
        return NULL;
    }

    if (PyCOND_INIT(&waiter->cond)) {
        PyMUTEX_FINI(&waiter->mutex);
        PyMem_RawFree(waiter);
        return NULL;
    }

    this_waiter = waiter;
    return waiter;
}

void
_PyParkingLot_DeinitThread(void)
{
    Waiter *waiter = this_waiter;
    if (waiter == NULL) {
        return;
    }

    if (--waiter->refcount != 0) {
        assert(waiter->refcount > 0);
        return;
    }

    this_waiter = NULL;
    PyMUTEX_FINI(&waiter->mutex);
    PyCOND_FINI(&waiter->cond);
    PyMem_RawFree(waiter);
}

struct Waiter *
_PyParkingLot_ThisWaiter(void)
{
    if (this_waiter == NULL) {
        return _PyParkingLot_InitThread();
    }
    return this_waiter;
}

static void
enqueue(Bucket *bucket, const void *key, Waiter *waiter,
        _PyTime_t start_time)
{
    /* initialize bucket */
    if (!bucket->root.next) {
        llist_init(&bucket->root);
    }

    waiter->key = (uintptr_t)key;
    waiter->time_to_be_fair = start_time + 1000*1000;
    llist_insert_tail(&bucket->root, &waiter->node);
    ++bucket->num_waiters;
}

static Waiter *
dequeue(Bucket *bucket, const void *key)
{
    struct llist_node *root = &bucket->root;
    struct llist_node *next = root->next;
    for (;;) {
        if (!next || next == root) {
            return NULL;
        }
        Waiter *waiter = llist_data(next, Waiter, node);
        if (waiter->key == (uintptr_t)key) {
            llist_remove(next);
            --bucket->num_waiters;
            return waiter;
        }
        next = next->next;
    }
}

int
_PySemaphore_Wait(Waiter *waiter, int64_t ns)
{
    PyThreadState *tstate = _PyThreadState_GET();
    int was_attached = 0;
    if (tstate) {
        was_attached = (_Py_atomic_load_int32(&tstate->status) == _Py_THREAD_ATTACHED);
        if (was_attached) {
            PyEval_ReleaseThread(tstate);
        }
        else {
            _PyEval_DropGIL(tstate);
        }
    }

    int res = PY_PARK_INTR;

    PyMUTEX_LOCK(&waiter->mutex);
    if (waiter->counter == 0) {
        int err;
        if (ns >= 0) {
            err = PyCOND_TIMEDWAIT(&waiter->cond, &waiter->mutex, ns / 1000);
        }
        else {
            err = PyCOND_WAIT(&waiter->cond, &waiter->mutex);
        }
        if (err) {
            res = PY_PARK_TIMEOUT;
        }
    }
    if (waiter->counter > 0) {
        waiter->counter--;
        res = PY_PARK_OK;
    }
    PyMUTEX_UNLOCK(&waiter->mutex);

    if (tstate) {
        if (was_attached) {
            PyEval_AcquireThread(tstate);
        }
        else {
            _PyEval_TakeGIL(tstate);
        }
    }
    return res;
}

void
_PySemaphore_Signal(Waiter *waiter, const char *msg, void *data)
{
    PyMUTEX_LOCK(&waiter->mutex);
    waiter->counter++;
    // waiter->last_notifier = _PyThreadState_GET();
    // waiter->last_notifier_msg = msg;
    // waiter->last_notifier_data = (void*)data;
    PyCOND_SIGNAL(&waiter->cond);
    PyMUTEX_UNLOCK(&waiter->mutex);
}

int
_PyParkingLot_ParkInt32(const int32_t *key, int32_t expected)
{
    Waiter *waiter = this_waiter;
    assert(waiter);

    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (_Py_atomic_load_int32(key) != expected) {
        _PyRawMutex_unlock(&bucket->mutex);
        return PY_PARK_AGAIN;
    }
    enqueue(bucket, key, waiter, 0);
    _PyRawMutex_unlock(&bucket->mutex);

    return _PySemaphore_Wait(waiter, -1);
}

int
_PyParkingLot_Park(const void *key, uintptr_t expected,
                    _PyTime_t start_time, int64_t ns)
{
    Waiter *waiter = this_waiter;
    assert(waiter);
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (_Py_atomic_load_uintptr(key) != expected) {
        _PyRawMutex_unlock(&bucket->mutex);
        return PY_PARK_AGAIN;
    }
    enqueue(bucket, key, waiter, start_time);
    _PyRawMutex_unlock(&bucket->mutex);

    int res = _PySemaphore_Wait(waiter, ns);
    if (res == PY_PARK_OK) {
        return res;
    }

    /* timeout or interrupt */
    _PyRawMutex_lock(&bucket->mutex);
    if (waiter->node.next == NULL) {
        _PyRawMutex_unlock(&bucket->mutex);
        /* We've been removed the waiter queue. Wait until we
         * receive process the wakup signal. */
        do {
            res = _PySemaphore_Wait(waiter, -1);
        } while (res != PY_PARK_OK);
        return PY_PARK_OK;
    }
    else {
        llist_remove(&waiter->node);
        --bucket->num_waiters;
    }
    _PyRawMutex_unlock(&bucket->mutex);
    return res;
}

void
_PyParkingLot_UnparkAll(const void *key)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    for (;;) {
        _PyRawMutex_lock(&bucket->mutex);
        Waiter *waiter = dequeue(bucket, key);
        _PyRawMutex_unlock(&bucket->mutex);

        if (!waiter) {
            return;
        }

        _PySemaphore_Signal(waiter, "_PyParkingLot_UnparkAll", NULL);
    }
}

void
_PyParkingLot_BeginUnpark(const void *key, Waiter **out,
                          int *more_waiters, int *should_be_fair)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);

    _PyTime_t now = _PyTime_GetMonotonicClock();
    Waiter *waiter = dequeue(bucket, key);

    *more_waiters = (bucket->num_waiters > 0);
    *should_be_fair = waiter ? now >= waiter->time_to_be_fair : 0;
    *out = waiter;
}

void
_PyParkingLot_FinishUnpark(const void *key, Waiter *waiter)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];
    _PyRawMutex_unlock(&bucket->mutex);

    if (waiter) {
        _PySemaphore_Signal(waiter, "_PyParkingLot_FinishUnpark", NULL);
    }
}

void
_PyParkingLot_AfterFork(void)
{
    /* After a fork only one thread remains. That thread cannot be blocked
     * so all entries in the parking lot are for dead threads.
     */
    memset(buckets, 0, sizeof(buckets));
}
