#include "Python.h"

#include "parking_lot.h"

#include "pycore_ceval.h"   // _PyEval_TakeGIL, _PyEval_DropGIL
#include "pycore_llist.h"
#include "pycore_pystate.h"

#define MAX_DEPTH 3

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

struct wait_entry {
    _PyWakeup *wakeup;
    struct llist_node node;
    uintptr_t key;
    void *data;
};

struct _PyWakeup {
#if defined(_WIN32)
    HANDLE sem;
#elif defined(USE_SEMAPHORES)
    sem_t sem;
#else
    PyMUTEX_T mutex;
    PyCOND_T cond;
    int counter;
#endif
};

typedef struct {
    Py_ssize_t refcount;
    uintptr_t thread_id;

    int depth;
    _PyWakeup semas[MAX_DEPTH];
} ThreadData;

#define NUM_BUCKETS 251

static Bucket buckets[NUM_BUCKETS];

Py_DECL_THREAD ThreadData *thread_data;

static void
_PyWakeup_Init(_PyWakeup *wakeup)
{
#if defined(_WIN32)
    wakeup->sem = CreateSemaphore(
        NULL,   //  attributes
        0,      //  initial count
        10,     //  maximum count
        NULL    //  unnamed
    );
    if (!wakeup->sem) {
        Py_FatalError("parking_lot: CreateSemaphore failed");
    }
#elif defined(USE_SEMAPHORES)
    if (sem_init(&wakeup->sem, /*pshared=*/0, /*value=*/0) < 0) {
        Py_FatalError("parking_lot: sem_init failed");
    }
#else
    if (pthread_mutex_init(&wakeup->mutex, NULL) != 0) {
        Py_FatalError("parking_lot: pthread_mutex_init failed");
    }
    if (pthread_cond_init(&wakeup->cond, NULL)) {
        Py_FatalError("parking_lot: pthread_cond_init failed");
    }
#endif
}

static void
_PyWakeup_Destroy(_PyWakeup *wakeup)
{
#if defined(_WIN32)
    CloseHandle(wakeup->sem);
#elif defined(USE_SEMAPHORES)
    sem_destroy(&wakeup->sem);
#else
    pthread_mutex_destroy(&wakeup->mutex);
    pthread_cond_destroy(&wakeup->cond);
#endif
}


static int
_PyWakeup_PlatformWait(_PyWakeup *wakeup, int64_t ns)
{
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
    wait = WaitForSingleObjectEx(wakeup->sem, millis, FALSE);
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

        err = sem_timedwait(&wakeup->sem, &ts);
    }
    else {
        err = sem_wait(&wakeup->sem);
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
    pthread_mutex_lock(&wakeup->mutex);
    if (wakeup->counter == 0) {
        int err;
        if (ns >= 0) {
            struct timespec ts;

            _PyTime_t deadline = _PyTime_GetSystemClock() + ns;
            _PyTime_AsTimespec(deadline, &ts);

            err = pthread_cond_timedwait(&wakeup->cond, &wakeup->mutex, &ts);
        }
        else {
            err = pthread_cond_wait(&wakeup->cond, &wakeup->mutex);
        }
        if (err) {
            res = PY_PARK_TIMEOUT;
        }
    }
    if (wakeup->counter > 0) {
        wakeup->counter--;
        res = PY_PARK_OK;
    }
    pthread_mutex_unlock(&wakeup->mutex);
#endif
    return res;
}

int
_PyWakeup_Wait(_PyWakeup *wakeup, int64_t ns, int detach)
{
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

    int res = _PyWakeup_PlatformWait(wakeup, ns);

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

_PyWakeup *
_PyWakeup_Acquire(void)
{
    // Make sure we have a valid thread_data. We need to acquire
    // some locks before we have a fully initialized PyThreadState.
    _PyParkingLot_InitThread();

    ThreadData *this_thread = thread_data;
    if (this_thread->depth >= MAX_DEPTH) {
        Py_FatalError("_PyWakeup_Acquire(): too many calls");
    }
    return &this_thread->semas[this_thread->depth++];
}

void
_PyWakeup_Release(_PyWakeup *wakeup)
{
    ThreadData *this_thread = thread_data;
    this_thread->depth--;
    if (&this_thread->semas[this_thread->depth] != wakeup) {
        Py_FatalError("_PyWakeup_Release(): mismatch wakeup");
    }
    _PyParkingLot_DeinitThread();
}

void
_PyWakeup_Wakeup(_PyWakeup *wakeup)
{
#if defined(_WIN32)
    if (!ReleaseSemaphore(wakeup->sem, 1, NULL)) {
        Py_FatalError("parking_lot: ReleaseSemaphore failed");
    }
#elif defined(USE_SEMAPHORES)
    int err = sem_post(&wakeup->sem);
    if (err != 0) {
        Py_FatalError("parking_lot: sem_post failed");
    }
#else
    pthread_mutex_lock(&wakeup->mutex);
    wakeup->counter++;
    pthread_cond_signal(&wakeup->cond);
    pthread_mutex_unlock(&wakeup->mutex);
#endif
}


void
_PyParkingLot_InitThread(void)
{
    if (thread_data != NULL) {
        thread_data->refcount++;
        return;
    }
    ThreadData *this_thread = PyMem_RawMalloc(sizeof(ThreadData));
    if (this_thread == NULL) {
        Py_FatalError("_PyParkingLot_InitThread: unable to allocate thread data");
    }
    memset(this_thread, 0, sizeof(*this_thread));
    this_thread->thread_id = _Py_ThreadId();
    this_thread->refcount = 1;
    this_thread->depth = 0;
    for (int i = 0; i < MAX_DEPTH; i++) {
        _PyWakeup_Init(&this_thread->semas[i]);
    }
    thread_data = this_thread;
}

void
_PyParkingLot_DeinitThread(void)
{
    ThreadData *td = thread_data;
    if (td == NULL) {
        return;
    }

    if (--td->refcount != 0) {
        assert(td->refcount > 0);
        return;
    }

    thread_data = NULL;
    for (int i = 0; i < MAX_DEPTH; i++) {
        _PyWakeup_Destroy(&td->semas[i]);
    }

    PyMem_RawFree(td);
}

static void
enqueue(Bucket *bucket, const void *key, struct wait_entry *wait)
{
    /* initialize bucket */
    if (!bucket->root.next) {
        llist_init(&bucket->root);
    }

    wait->key = (uintptr_t)key;
    llist_insert_tail(&bucket->root, &wait->node);
    ++bucket->num_waiters;
}

static struct wait_entry *
dequeue(Bucket *bucket, const void *key)
{
    struct llist_node *root = &bucket->root;
    struct llist_node *next = root->next;
    for (;;) {
        if (!next || next == root) {
            return NULL;
        }
        struct wait_entry *wait = llist_data(next, struct wait_entry, node);
        if (wait->key == (uintptr_t)key) {
            llist_remove(next);
            --bucket->num_waiters;
            return wait;
        }
        next = next->next;
    }
}

typedef int (*_Py_validate_func)(const void *, const void *);

static int
_PyParkingLot_ParkEx(const void *key,
                     _Py_validate_func validate,
                     const void *expected,
                     struct wait_entry *wait,
                     int64_t ns,
                     int detach)
{
    ThreadData *this_thread = thread_data;
    assert(thread_data->depth >= 0 && thread_data->depth < MAX_DEPTH);
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);
    if (!validate(key, expected)) {
        _PyRawMutex_unlock(&bucket->mutex);
        return PY_PARK_AGAIN;
    }
    wait->wakeup = _PyWakeup_Acquire();
    enqueue(bucket, key, wait);
    _PyRawMutex_unlock(&bucket->mutex);

    int res = _PyWakeup_Wait(wait->wakeup, ns, detach);
    if (res == PY_PARK_OK) {
        this_thread->depth--;
        return res;
    }

    /* timeout or interrupt */
    _PyRawMutex_lock(&bucket->mutex);
    if (wait->node.next == NULL) {
        _PyRawMutex_unlock(&bucket->mutex);
        /* We've been removed the waiter queue. Wait until we
         * receive process the wakup signal. */
        do {
            res = _PyWakeup_Wait(wait->wakeup, -1, detach);
        } while (res != PY_PARK_OK);
        _PyWakeup_Release(wait->wakeup);
        return PY_PARK_OK;
    }
    else {
        llist_remove(&wait->node);
        --bucket->num_waiters;
    }
    _PyRawMutex_unlock(&bucket->mutex);
    _PyWakeup_Release(wait->wakeup);
    return res;
}

static int
validate_int(const void *key, const void *expected_ptr)
{
    int expected = *(const int *)expected_ptr;
    return _Py_atomic_load_int(key) == expected;
}

int
_PyParkingLot_ParkInt(const int *key, int expected, int detach)
{
    struct wait_entry wait;
    return _PyParkingLot_ParkEx(
        key, &validate_int, &expected, &wait, -1, detach);
}

static int
validate_ptr(const void *key, const void *expected_ptr)
{
    uintptr_t expected = *(const uintptr_t *)expected_ptr;
    return _Py_atomic_load_uintptr(key) == expected;
}

int
_PyParkingLot_Park(const void *key, uintptr_t expected,
                   void *data, int64_t ns)
{
    struct wait_entry wait;
    wait.data = data;
    return _PyParkingLot_ParkEx(
        key, &validate_ptr, &expected, &wait, ns, /*detach=*/1);
}

static int
validate_uint8(const void *key, const void *expected_ptr)
{
    uint8_t expected = *(const uint8_t *)expected_ptr;
    return _Py_atomic_load_uint8(key) == expected;
}

int
_PyParkingLot_ParkUint8(const uint8_t *key, uint8_t expected,
                        void *data, int64_t ns, int detach)
{
    struct wait_entry wait;
    wait.data = data;
    return _PyParkingLot_ParkEx(
        key, &validate_uint8, &expected, &wait, ns, detach);
}

void
_PyParkingLot_UnparkAll(const void *key)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    for (;;) {
        _PyRawMutex_lock(&bucket->mutex);
        struct wait_entry *entry = dequeue(bucket, key);
        _PyRawMutex_unlock(&bucket->mutex);

        if (!entry) {
            return;
        }

        _PyWakeup_Wakeup(entry->wakeup);
    }
}

void *
_PyParkingLot_BeginUnpark(const void *key,
                          struct wait_entry **out_waiter,
                          int *more_waiters)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];

    _PyRawMutex_lock(&bucket->mutex);

    struct wait_entry *waiter = dequeue(bucket, key);

    *more_waiters = (bucket->num_waiters > 0);
    *out_waiter = waiter;
    return waiter ? waiter->data : NULL;
}

void
_PyParkingLot_FinishUnpark(const void *key, struct wait_entry *entry)
{
    Bucket *bucket = &buckets[((uintptr_t)key) % NUM_BUCKETS];
    _PyRawMutex_unlock(&bucket->mutex);

    if (entry) {
        _PyWakeup_Wakeup(entry->wakeup);
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
