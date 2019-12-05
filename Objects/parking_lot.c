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

enum {
    SEMA_TIMEOUT = 0,
    SEMA_OK = 1,
};

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
_PySemaphore_Wait(PyThreadStateOS *os, int detach, int64_t ns)
{
    PyThreadState *tstate = NULL;
    if (detach) {
        tstate = PyEval_SaveThread();
    }

    PyMUTEX_LOCK(&os->waiter_mutex);
    while (os->waiter_counter == 0) {
        if (ns >= 0) {
            int ret = PyCOND_TIMEDWAIT(&os->waiter_cond, &os->waiter_mutex, ns / 1000);
            if (ret) {
                /* timeout */
                PyMUTEX_UNLOCK(&os->waiter_mutex);
                if (detach) {
                    PyEval_RestoreThread(tstate);
                }
                return SEMA_TIMEOUT;
            }
        }
        else {
            PyCOND_WAIT(&os->waiter_cond, &os->waiter_mutex);
        }
    }
    os->waiter_counter--;
    PyMUTEX_UNLOCK(&os->waiter_mutex);

    if (detach) {
        PyEval_RestoreThread(tstate);
    }
    return SEMA_OK;
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
_PyParkingLot_Park(const uintptr_t *key, uintptr_t expected,
                   _PyTime_t start_time, int64_t ns)
{
    PyThreadState *tstate = _PyThreadState_GET();
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (_Py_atomic_load_uintptr(key) != expected) {
        _PyRawMutex_unlock(&bucket->mutex);
        return -1;
    }
    enqueue(bucket, key, tstate, start_time);
    _PyRawMutex_unlock(&bucket->mutex);

    int res = _PySemaphore_Wait(tstate->os, DETACH, ns);
    if (res == SEMA_TIMEOUT) {
        /* timeout */
        _PyRawMutex_lock(&bucket->mutex);

        struct PyThreadStateWaiter *waiter = &tstate->os->waiter;
        if (waiter->next == NULL) {
            _PyRawMutex_unlock(&bucket->mutex);
            res = _PySemaphore_Wait(tstate->os, DETACH, -1);
            assert(res == SEMA_OK);
            return 0;
        }
        else {
            unlink_waiter(bucket, waiter);
        }

        _PyRawMutex_unlock(&bucket->mutex);
        return -2;
    }
    return 0;
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
_PyParkingLot_BeginUnpark(const void *key, PyThreadStateOS **out_os,
                          int *more_waiters, int *time_to_be_fair)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);

    _PyTime_t now = _PyTime_GetMonotonicClock();
    PyThreadStateOS *os = dequeue(bucket, key);

    *out_os = os;
    *more_waiters = (bucket->num_waiters > 0);
    if (os) {
        *time_to_be_fair = now >= os->waiter.time_to_be_fair;
    } else {
        *time_to_be_fair = 0;
    }
}

void
_PyParkingLot_FinishUnpark(const void *key, PyThreadStateOS *os)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];
    _PyRawMutex_unlock(&bucket->mutex);

    if (os) {
        _PySemaphore_Signal(os, "_PyParkingLot_UnparkOne", NULL);
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