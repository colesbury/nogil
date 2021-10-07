#ifndef Py_ATOMIC_MSC_H
#  error "this header file must not be included directly"
#endif


#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

// TODO(sgross): should the uintptr functions use ExchangePointer? It's unclear whether the
// values have to be valid aligned pointers or just the addresses. Also load needs to be
// updated for ARM synchronization.


static inline int32_t
_Py_atomic_add_int32(volatile int32_t *address, int32_t value)
{
    return (int32_t)_InterlockedExchangeAdd((volatile LONG*)address, (LONG)value);
}

static inline int64_t
_Py_atomic_add_int64(volatile int64_t *address, int64_t value)
{
    return (int64_t)_InterlockedExchangeAdd64((volatile LONG64*)address, (LONG64)value);
}

static inline intptr_t
_Py_atomic_add_intptr(volatile intptr_t *address, intptr_t value)
{
#if SIZEOF_UINTPTR_T == 8
    return (intptr_t)_InterlockedExchangeAdd64((volatile LONG64*)address, (LONG64)value);
#else
    return (intptr_t)_InterlockedExchangeAdd((volatile LONG*)address, (LONG)value);
#endif
}

static inline uint32_t
_Py_atomic_add_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile LONG*)address, (LONG)value);
}

static inline uint64_t
_Py_atomic_add_uint64(volatile uint64_t *address, uint64_t value)
{
    return (uint64_t)_InterlockedExchangeAdd64((volatile LONG64*)address, (LONG64)value);
}

static inline uintptr_t
_Py_atomic_add_uintptr(volatile uintptr_t *address, uintptr_t value)
{
#if SIZEOF_UINTPTR_T == 8
    return (uintptr_t)_InterlockedExchangeAdd64((volatile LONG64*)address, (LONG64)value);
#else
    return (uintptr_t)_InterlockedExchangeAdd((volatile LONG*)address, (LONG)value);
#endif
}


static inline int
_Py_atomic_compare_exchange_int32(volatile int32_t *address, int32_t expected, int32_t value)
{
    return (LONG)expected == _InterlockedCompareExchange((volatile LONG*)address, (LONG)value, (LONG)expected);
}

static inline int
_Py_atomic_compare_exchange_int64(volatile int64_t *address, int64_t expected, int64_t value)
{
    return (LONG64)expected == _InterlockedCompareExchange64((volatile LONG64*)address, (LONG64)value, (LONG64)expected);
}

static inline int
_Py_atomic_compare_exchange_intptr(volatile intptr_t *address, intptr_t expected, intptr_t value)
{
    return (PVOID)expected == _InterlockedCompareExchangePointer((volatile PVOID*)address, (PVOID)value, (PVOID)expected);
}

static inline int
_Py_atomic_compare_exchange_uint32(volatile uint32_t *address, uint32_t expected, uint32_t value)
{
    return (LONG)expected == _InterlockedCompareExchange((volatile LONG*)address, (LONG)value, (LONG)expected);
}

static inline int
_Py_atomic_compare_exchange_uint64(volatile uint64_t *address, uint64_t expected, uint64_t value)
{
    return (LONG64)expected == _InterlockedCompareExchange64((volatile LONG64*)address, (LONG64)value, (LONG64)expected);
}

static inline int
_Py_atomic_compare_exchange_uintptr(volatile uintptr_t *address, uintptr_t expected, uintptr_t value)
{
    return (PVOID)expected == _InterlockedCompareExchangePointer((volatile PVOID*)address, (PVOID)value, (PVOID)expected);
}

static inline int
_Py_atomic_compare_exchange_ptr(volatile void *address, void *expected, void *value)
{
    return (PVOID)expected == _InterlockedCompareExchangePointer((volatile PVOID*)address, (PVOID)value, (PVOID)expected);
}


static inline int32_t
_Py_atomic_exchange_int32(volatile int32_t *address, int32_t value)
{
    return (int32_t)_InterlockedExchange((volatile LONG*)address, (LONG)value);
}

static inline int64_t
_Py_atomic_exchange_int64(volatile int64_t *address, int64_t value)
{
    return (int64_t)_InterlockedExchange64((volatile LONG64*)address, (LONG64)value);
}

static inline intptr_t
_Py_atomic_exchange_intptr(volatile intptr_t *address, intptr_t value)
{
    return (intptr_t)_InterlockedExchangePointer((volatile PVOID*)address, (PVOID)value);

}

static inline uint32_t
_Py_atomic_exchange_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedExchange((volatile LONG*)address, (LONG)value);
}

static inline uint64_t
_Py_atomic_exchange_uint64(volatile uint64_t *address, uint64_t value)
{
    return (uint64_t)_InterlockedExchange64((volatile LONG64*)address, (LONG64)value);
}

static inline uintptr_t
_Py_atomic_exchange_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return (uintptr_t)_InterlockedExchangePointer((volatile PVOID*)address, (PVOID)value);
}

static inline void *
_Py_atomic_exchange_ptr(volatile void *address, void *value)
{
    return (void *)_InterlockedExchangePointer((volatile PVOID*)address, (PVOID)value);
}


static inline int32_t
_Py_atomic_load_int32(volatile int32_t *address)
{
    return *address;
}

static inline int64_t
_Py_atomic_load_int64(volatile int64_t *address)
{
    return *address;
}

static inline intptr_t
_Py_atomic_load_intptr(volatile intptr_t *address)
{
    return *address;
}

static inline uint32_t
_Py_atomic_load_uint32(volatile uint32_t *address)
{
    return *address;
}

static inline uint64_t
_Py_atomic_load_uint64(volatile uint64_t *address)
{
    return *address;
}

static inline uintptr_t
_Py_atomic_load_uintptr(volatile uintptr_t *address)
{
    return *address;
}

static inline void *
_Py_atomic_load_ptr(volatile void *address)
{
    return *(volatile void**)address;
}


static inline void
_Py_atomic_store_int32(volatile int32_t *address, int32_t value)
{
    _InterlockedExchange((volatile LONG*)address, (LONG)value);
}

static inline void
_Py_atomic_store_int64(volatile int64_t *address, int64_t value)
{
    _InterlockedExchange((volatile LONG64*)address, (LONG64)value);
}

static inline void
_Py_atomic_store_intptr(volatile intptr_t *address, intptr_t value)
{
    _InterlockedExchangePointer((volatile PVOID*)address, (PVOID)value);
}

static inline void
_Py_atomic_store_uint32(volatile uint32_t *address, uint32_t value)
{
    _InterlockedExchange((volatile LONG*)address, (LONG)value);
}

static inline void
_Py_atomic_store_uint64(volatile uint64_t *address, uint64_t value)
{
    _InterlockedExchange((volatile LONG64*)address, (LONG64)value);
}

static inline void
_Py_atomic_store_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    _InterlockedExchangePointer((volatile PVOID*)address, (PVOID)value);
}

static inline void
_Py_atomic_store_ptr(volatile void *address, void *value)
{
    _InterlockedExchangePointer((void * volatile *)address, (void *)value);
}


// FIXME: rename to _Py_atomic_store_uint8_relaxed
static inline void
_Py_atomic_store_uint8(volatile uint8_t* address, uint8_t value)
{
    *address = value;
}

// FIXME: rename to _Py_atomic_store_ssize_relaxed
static inline void
_Py_atomic_store_ssize(volatile Py_ssize_t* address, Py_ssize_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_int32_relaxed(volatile int32_t* address, int32_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_int64_relaxed(volatile int64_t* address, int64_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_intptr_relaxed(volatile intptr_t* address, intptr_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uint32_relaxed(volatile uint32_t* address, uint32_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uint64_relaxed(volatile uint64_t* address, uint64_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uint64_release(volatile uint64_t* address, uint64_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uintptr_relaxed(volatile uintptr_t* address, uintptr_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_ptr_relaxed(volatile void* address, void* value)
{
    *(void * volatile *)address = value;
}

static inline void
_Py_atomic_store_ptr_release(volatile void* address, void* value)
{
    *(void * volatile *)address = value;
}

static inline void
_Py_atomic_store_ssize_relaxed(volatile Py_ssize_t* address, Py_ssize_t value)
{
    *address = value;
}

static inline void
_Py_atomic_thread_fence(void)
{
    __faststorefence();
}