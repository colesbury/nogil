#include "Python.h"

#include "pycore_pystate.h"
#include "Python/condvar.h"

#include "parking_lot.h"
#include "pyatomic.h"

typedef struct {
    _PyRawMutex mutex;

    struct PyThreadStateWaiter root;
    size_t num_waiters;
} Bucket;

#define NUM_BUCKETS 251

static Bucket buckets[NUM_BUCKETS];

static void
link_waiter(Bucket *bucket, struct PyThreadStateWaiter *node)
{
    struct PyThreadStateWaiter *root = &bucket->root;

    node->prev = root->prev;
    node->next = root;
    root->prev->next = node;
    root->prev = node;

    ++bucket->num_waiters;
}

static void
unlink_waiter(Bucket *bucket, struct PyThreadStateWaiter *node)
{
    struct PyThreadStateWaiter *prev = node->prev;
    struct PyThreadStateWaiter *next = node->next;
    prev->next = next;
    next->prev = prev;
    node->prev = NULL;
    node->next = NULL;

    --bucket->num_waiters;
}

static void
enqueue(Bucket *bucket, const void *key, PyThreadState *tstate,
        _PyTime_t start_time)
{
    /* initialize bucket */
    struct PyThreadStateWaiter *root = &bucket->root;
    if (root->next == NULL) {
        root->next = root;
        root->prev = root;
    }

    struct PyThreadStateWaiter *node = &tstate->os->waiter;
    node->key = (uintptr_t)key;
    node->time_to_be_fair = start_time + 1000*1000;
    link_waiter(bucket, node);
}

static PyThreadStateOS *
dequeue(Bucket *bucket, const void *key)
{
    struct PyThreadStateWaiter *root = &bucket->root;
    struct PyThreadStateWaiter *waiter = root->next;
    for (;;) {
        if (!waiter || waiter == root) {
            return NULL;
        }
        if (waiter->key == (uintptr_t)key) {
            unlink_waiter(bucket, waiter);
            return (PyThreadStateOS *)waiter;
        }
        waiter = waiter->next;
    }
}

int
_PySemaphore_Wait(PyThreadState *tstate, int64_t ns)
{
    PyThreadStateOS *os = tstate->os;
    int was_attached =
        (_Py_atomic_load_int32(&tstate->status) == _Py_THREAD_ATTACHED);

    if (was_attached) {
        PyEval_ReleaseThread(tstate);
    }
    else {
        _PyEval_DropGIL(tstate);
    }

    int res = PY_PARK_INTR;

    PyMUTEX_LOCK(&os->waiter_mutex);
    if (os->waiter_counter == 0) {
        int err;
        if (ns >= 0) {
            err = PyCOND_TIMEDWAIT(&os->waiter_cond, &os->waiter_mutex, ns / 1000);
        }
        else {
            err = PyCOND_WAIT(&os->waiter_cond, &os->waiter_mutex);
        }
        if (err) {
            res = PY_PARK_TIMEOUT;
        }
    }
    if (os->waiter_counter > 0) {
        os->waiter_counter--;
        res = PY_PARK_OK;
    }
    PyMUTEX_UNLOCK(&os->waiter_mutex);

    if (was_attached) {
        PyEval_AcquireThread(tstate);
    }
    else {
        _PyEval_TakeGIL(tstate);
    }
    return res;
}

void
_PySemaphore_Signal(PyThreadStateOS *os, const char *msg, void *data)
{
    PyMUTEX_LOCK(&os->waiter_mutex);
    os->waiter_counter++;
    os->last_notifier = _PyThreadState_GET();
    os->last_notifier_msg = msg;
    os->last_notifier_data = (void*)data;
    os->counter++;
    PyCOND_SIGNAL(&os->waiter_cond);
    PyMUTEX_UNLOCK(&os->waiter_mutex);
}

int
_PyParkingLot_ParkInt32(const int32_t *key, int32_t expected)
{
    PyThreadState *tstate = _PyThreadState_GET();
    assert(tstate);

    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (_Py_atomic_load_int32(key) != expected) {
        _PyRawMutex_unlock(&bucket->mutex);
        return PY_PARK_AGAIN;
    }
    enqueue(bucket, key, tstate, 0);
    _PyRawMutex_unlock(&bucket->mutex);

    return _PySemaphore_Wait(tstate, -1);
}

int
_PyParkingLot_Park(const uintptr_t *key, uintptr_t expected,
                   _PyTime_t start_time, int64_t ns)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (_Py_atomic_load_uintptr(key) != expected) {
        _PyRawMutex_unlock(&bucket->mutex);
        return PY_PARK_AGAIN;
    }
    enqueue(bucket, key, tstate, start_time);
    _PyRawMutex_unlock(&bucket->mutex);

    int res = _PySemaphore_Wait(tstate, ns);
    if (res == PY_PARK_OK) {
        return res;
    }

    /* timeout or interrupt */
    _PyRawMutex_lock(&bucket->mutex);
    struct PyThreadStateWaiter *waiter = &tstate->os->waiter;
    if (waiter->next == NULL) {
        _PyRawMutex_unlock(&bucket->mutex);
        /* We've been removed the waiter queue. Wait until we
         * receive process the wakup signal. */
        do {
            res = _PySemaphore_Wait(tstate, -1);
        } while (res != PY_PARK_OK);
        return PY_PARK_OK;
    }
    else {
        unlink_waiter(bucket, waiter);
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
        PyThreadStateOS *os = dequeue(bucket, key);
        _PyRawMutex_unlock(&bucket->mutex);

        if (!os) {
            return;
        }

        _PySemaphore_Signal(os, "_PyParkingLot_UnparkAll", NULL);
    }
}

void
_PyParkingLot_BeginUnpark(const void *key, PyThreadState **out_tstate,
                          int *more_waiters, int *time_to_be_fair)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);

    _PyTime_t now = _PyTime_GetMonotonicClock();
    PyThreadStateOS *os = dequeue(bucket, key);

    *more_waiters = (bucket->num_waiters > 0);
    if (os) {
        *time_to_be_fair = now >= os->waiter.time_to_be_fair;
        *out_tstate = os->tstate;
    } else {
        *time_to_be_fair = 0;
        *out_tstate = NULL;
    }
}

void
_PyParkingLot_FinishUnpark(const void *key, PyThreadState *tstate)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];
    _PyRawMutex_unlock(&bucket->mutex);

    if (tstate) {
        _PySemaphore_Signal(tstate->os, "_PyParkingLot_UnparkOne", NULL);
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