#ifndef Py_PARKING_LOT_H
#define Py_PARKING_LOT_H

#include "pyatomic.h"
#include "pycore_llist.h"
#include "pycore_condvar.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PY_PARK_AGAIN = -1,
    PY_PARK_TIMEOUT = -2,
    PY_PARK_INTR = -3,
    PY_PARK_OK = 0,
};

typedef struct Waiter {
    struct llist_node node;     // wait queue node
    Py_ssize_t refcount;
    struct Waiter *next_waiter; // for "raw" locks
    PyMUTEX_T mutex;
    PyCOND_T cond;
    int counter;
    uintptr_t key;
    int64_t time_to_be_fair;
    uintptr_t thread_id;
    uintptr_t handoff_elem;
} Waiter;

Waiter *
_PyParkingLot_InitThread(void);

void
_PyParkingLot_DeinitThread(void);

PyAPI_FUNC(Waiter *)
_PyParkingLot_ThisWaiter(void);

PyAPI_FUNC(void)
_PySemaphore_Signal(Waiter *waiter, const char *msg, void *data);

PyAPI_FUNC(int)
_PySemaphore_Wait(Waiter *waiter, int64_t ns);

PyAPI_FUNC(int)
_PyParkingLot_ParkInt32(const int32_t *key, int32_t expected);

PyAPI_FUNC(int)
_PyParkingLot_Park(const void *key, uintptr_t expected,
                   _PyTime_t start_time, int64_t ns);

PyAPI_FUNC(void)
_PyParkingLot_UnparkAll(const void *key);

PyAPI_FUNC(void)
_PyParkingLot_BeginUnpark(const void *key, struct Waiter **out,
                          int *more_waiters, int *should_be_fair);

PyAPI_FUNC(void)
_PyParkingLot_FinishUnpark(const void *key, struct Waiter *waiter);

PyAPI_FUNC(void)
_PyParkingLot_AfterFork(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PARKING_LOT_H */
