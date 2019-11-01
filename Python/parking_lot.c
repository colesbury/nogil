#include "Python.h"

#include "parking_lot.h"

#include "lock.h"
#include "pyatomic.h"
#include "pycore_llist.h"
#include "pycore_pystate.h"


#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif (defined(_POSIX_SEMAPHORES) && !defined(HAVE_BROKEN_POSIX_SEMAPHORES) && \
     defined(HAVE_SEM_TIMEDWAIT))
#define USE_SEMAPHORES
#include <semaphore.h>
#else
#include <pthread.h>
#endif

typedef struct {
    _PyRawMutex mutex;

    struct llist_node root;
    size_t num_waiters;
} Bucket;

// A Waiter augmented with platform-specific semaphore
typedef struct {
    Waiter w;

    // wait queue node
    struct llist_node node;

#if defined(_WIN32)
    HANDLE sem;
#elif defined(USE_SEMAPHORES)
    sem_t sem;
#else
    PyMUTEX_T mutex;
    PyCOND_T cond;
    int counter;
#endif
} WaiterImpl;

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
    WaiterImpl *waiter = PyMem_RawMalloc(sizeof(WaiterImpl));
    if (waiter == NULL) {
        return NULL;
    }
    memset(waiter, 0, sizeof(*waiter));
    waiter->w.thread_id = _Py_ThreadId();
    waiter->w.refcount++;
#if defined(_WIN32)
    waiter->sem = CreateSemaphore(
        NULL,   //  attributes
        0,      //  initial count
        1,      //  maximum count
        NULL    //  unnamed
    );
#elif defined(USE_SEMAPHORES)
    if (sem_init(&waiter->sem, /*pshared=*/0, /*value=*/0) < 0) {
        PyMem_RawFree(waiter);
        return NULL;
    }
#else
    if (pthread_mutex_init(&waiter->mutex, NULL) != 0) {
        PyMem_RawFree(waiter);
        return NULL;
    }
    if (pthread_cond_init(&waiter->cond, NULL)) {
        pthread_mutex_destroy(&waiter->mutex);
        PyMem_RawFree(waiter);
        return NULL;
    }
#endif

    this_waiter = (Waiter *)waiter;
    return (Waiter *)waiter;
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

    WaiterImpl *w = (WaiterImpl *)waiter;
#if defined(_WIN32)
    CloseHandle(w->sem);
#elif defined(USE_SEMAPHORES)
    sem_destroy(&w->sem);
#else
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
#endif

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
    llist_insert_tail(&bucket->root, &((WaiterImpl *)waiter)->node);
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
        WaiterImpl *waiter = llist_data(next, WaiterImpl, node);
        if (waiter->w.key == (uintptr_t)key) {
            llist_remove(next);
            --bucket->num_waiters;
            return (Waiter *)waiter;
        }
        next = next->next;
    }
}

int
_PySemaphore_Wait(Waiter *waiter, int64_t ns)
{
    return _PySemaphore_WaitEx(waiter, ns, /*deatch=*/1);
}

int
_PySemaphore_WaitEx(Waiter *waiter, int64_t ns, int detach)
{
    WaiterImpl *w = (WaiterImpl *)waiter;
    PyThreadState *tstate = _PyThreadState_GET();
    int was_attached = 0;
    if (tstate) {
        was_attached = (_Py_atomic_load_int32(&tstate->status) == _Py_THREAD_ATTACHED);
        if (was_attached && detach) {
            PyEval_ReleaseThread(tstate);
        }
        else {
            _PyEval_DropGIL(tstate);
        }
    }

    int res = PY_PARK_INTR;
#if defined(_WIN32)
    DWORD wait;
    DWORD millis = 0;
    if (ns < 0) {
        millis = INFINITE;
    }
    else {
        millis = (DWORD) (ns / 1000000);
    }
    wait = WaitForSingleObjectEx(w->sem, millis, FALSE);
    if (wait == WAIT_OBJECT_0) {
        res = PY_PARK_OK;
    }
    else if (wait == WAIT_TIMEOUT) {
        res = PY_PARK_TIMEOUT;
    }
#elif defined(USE_SEMAPHORES)
    int err;
    if (ns >= 0) {
        struct timespec ts;

        _PyTime_t deadline = _PyTime_GetSystemClock() + ns;
        _PyTime_AsTimespec(deadline, &ts);

        err = sem_timedwait(&w->sem, &ts);
    }
    else {
        err = sem_wait(&w->sem);
    }
    if (err == -1) {
        err = errno;
        if (err == EINTR) {
            res = PY_PARK_INTR;
        }
        else if (err == ETIMEDOUT) {
            res = PY_PARK_TIMEOUT;
        }
        else {
            _Py_FatalErrorFormat(__func__,
                "unexpected error from semaphore: %d",
                err);
        }
    }
    else {
        res = PY_PARK_OK;
    }
#else
    pthread_mutex_lock(&w->mutex);
    if (w->counter == 0) {
        int err;
        if (ns >= 0) {
            struct timespec ts;

            _PyTime_t deadline = _PyTime_GetSystemClock() + ns;
            _PyTime_AsTimespec(deadline, &ts);

            err = pthread_cond_timedwait(&w->cond, &w->mutex, &ts);
        }
        else {
            err = pthread_cond_wait(&w->cond, &w->mutex);
        }
        if (err) {
            res = PY_PARK_TIMEOUT;
        }
    }
    if (w->counter > 0) {
        w->counter--;
        res = PY_PARK_OK;
    }
    pthread_mutex_unlock(&w->mutex);
#endif

    if (tstate) {
        if (was_attached && detach) {
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
    // waiter->last_notifier = _PyThreadState_GET();
    // waiter->last_notifier_msg = msg;
    // waiter->last_notifier_data = (void*)data;
    WaiterImpl *w = (WaiterImpl *)waiter;
#if defined(_WIN32)
    ReleaseSemaphore(&w->sem, 1, NULL);
#error "NYI"
#elif defined(USE_SEMAPHORES)
    int err;
    err = sem_post(&w->sem);
    assert(err == 0); (void)err;
#else
    pthread_mutex_lock(&w->mutex);
    w->counter++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
#endif
}

typedef int (*_Py_validate_func)(const void *, const void *);

int
_PyParkingLot_ParkEx(const void *key,
                     _Py_validate_func validate,
                     const void *expected,
                     _PyTime_t start_time,
                     int64_t ns)
{
    Waiter *waiter = this_waiter;
    assert(waiter);
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (!validate(key, expected)) {
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
    WaiterImpl *w = (WaiterImpl *)waiter;
    if (w->node.next == NULL) {
        _PyRawMutex_unlock(&bucket->mutex);
        /* We've been removed the waiter queue. Wait until we
         * receive process the wakup signal. */
        do {
            res = _PySemaphore_Wait(waiter, -1);
        } while (res != PY_PARK_OK);
        return PY_PARK_OK;
    }
    else {
        llist_remove(&w->node);
        --bucket->num_waiters;
    }
    _PyRawMutex_unlock(&bucket->mutex);
    return res;
}

static int
validate_int32(const void *key, const void *expected_ptr)
{
    int32_t expected = *(const int32_t *)expected_ptr;
    return _Py_atomic_load_int32(key) == expected;
}

int
_PyParkingLot_ParkInt32(const int32_t *key, int32_t expected)
{
    return _PyParkingLot_ParkEx(
        key, &validate_int32, &expected, 0, -1);
}

static int
validate_ptr(const void *key, const void *expected_ptr)
{
    uintptr_t expected = *(const uintptr_t *)expected_ptr;
    return _Py_atomic_load_uintptr(key) == expected;
}

int
_PyParkingLot_Park(const void *key, uintptr_t expected,
                    _PyTime_t start_time, int64_t ns)
{
    return _PyParkingLot_ParkEx(
        key, &validate_ptr, &expected, start_time, ns);
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
