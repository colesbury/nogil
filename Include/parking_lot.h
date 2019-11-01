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

typedef struct _PyWakeup _PyWakeup;

typedef struct _PyUnpark {
    void *data;
    void *wait_entry;
    int more_waiters;
} _PyUnpark;

struct wait_entry;

void
_PyParkingLot_InitThread(void);

void
_PyParkingLot_DeinitThread(void);

PyAPI_FUNC(_PyWakeup *)
_PyWakeup_Acquire(void);

PyAPI_FUNC(void)
_PyWakeup_Release(_PyWakeup *wakeup);

PyAPI_FUNC(void)
_PyWakeup_Wakeup(_PyWakeup *wakeup);

PyAPI_FUNC(int)
_PyWakeup_Wait(_PyWakeup *wakeup, int64_t ns, int detach);

PyAPI_FUNC(int)
_PyParkingLot_ParkInt(const int *key, int expected, int detach);

PyAPI_FUNC(int)
_PyParkingLot_Park(const void *key, uintptr_t expected,
                   void *data, int64_t ns);

PyAPI_FUNC(int)
_PyParkingLot_ParkUint8(const uint8_t *key, uint8_t expected,
                        void *data, int64_t ns, int detach);

PyAPI_FUNC(void)
_PyParkingLot_UnparkAll(const void *key);

PyAPI_FUNC(void *)
_PyParkingLot_BeginUnpark(const void *key,
                          struct wait_entry **out_waiter,
                          int *more_waiters);

PyAPI_FUNC(void)
_PyParkingLot_FinishUnpark(const void *key, struct wait_entry *waiter);

PyAPI_FUNC(void)
_PyParkingLot_AfterFork(void);


#ifdef __cplusplus
}
#endif
#endif /* !Py_PARKING_LOT_H */
