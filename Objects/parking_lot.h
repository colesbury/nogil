#ifndef Py_PARKING_LOT_H
#define Py_PARKING_LOT_H

#include "pyatomic.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PY_PARK_AGAIN = -1,
    PY_PARK_TIMEOUT = -2,
    PY_PARK_INTR = -3,
    PY_PARK_OK = 0,
};

int
_PySemaphore_Wait(PyThreadState *tstate, int64_t ns);

void
_PySemaphore_Signal(PyThreadStateOS *os, const char *msg, void *data);

/* Functions for waking and parking threads */
int
_PyParkingLot_ParkInt32(const int32_t *key, int32_t expected);

int
_PyParkingLot_Park(const uintptr_t *key, uintptr_t expected,
                   _PyTime_t start_time, int64_t timeout_ns);

void
_PyParkingLot_UnparkAll(const void *key);

void
_PyParkingLot_BeginUnpark(const void *key, PyThreadState **tstate,
                          int *more_waiters, int *time_to_be_fair);

void
_PyParkingLot_FinishUnpark(const void *key, PyThreadState *tstate);

void
_PyParkingLot_AfterFork(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PARKING_LOT_H */
