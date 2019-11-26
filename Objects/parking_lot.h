#ifndef Py_PARKING_LOT_H
#define Py_PARKING_LOT_H

#include "pyatomic.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DONT_DETACH = 0,
    DETACH = 1
};

int
_PySemaphore_Wait(PyThreadStateOS *os, int detach, int64_t ns);

void
_PySemaphore_Signal(PyThreadStateOS *os, const char *msg, void *data);

/* Functions for waking and parking threads */
int
_PyParkingLot_Park(const uintptr_t *key, uintptr_t expected,
				   _PyTime_t start_time);

void
_PyParkingLot_BeginUnpark(const void *key, PyThreadStateOS **os,
                          int *more_waiters, int *time_to_be_fair);

void
_PyParkingLot_FinishUnpark(const void *key, PyThreadStateOS *os);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PARKING_LOT_H */
