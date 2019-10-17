#ifndef Py_ATOMIC_MSC_H
#  error "this header file must not be included directly"
#endif

#include <intrin.h>

// TODO(sgross): should the uintptr functions use ExchangePointer? It's unclear whether the
// values have to be valid aligned pointers or just the addresses. Also load needs to be
// updated for ARM synchronization.


static inline int
_Py_atomic_add_int(volatile int *address, int value)
{
    return (int)_InterlockedExchangeAdd((volatile long*)address, (long)value);
}

static inline int32_t
_Py_atomic_add_int32(volatile int32_t *address, int32_t value)
{
    return (int32_t)_InterlockedExchangeAdd((volatile long*)address, (long)value);
}

static inline int64_t
_Py_atomic_add_int64(volatile int64_t *address, int64_t value)
{
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int64_t)_InterlockedExchangeAdd64((volatile __int64*)address, (__int64)value);
#else
    for (;;) {
        int64_t old_value = *address;
        int64_t new_value = old_value + value;
        if (_InterlockedCompareExchange64((volatile __int64*)address, (__int64)new_value, (__int64)old_value)) {
            return old_value;
        }
    }
#endif
}

static inline intptr_t
_Py_atomic_add_intptr(volatile intptr_t *address, intptr_t value)
{
#if SIZEOF_VOID_P == 8
    return (intptr_t)_InterlockedExchangeAdd64((volatile __int64*)address, (__int64)value);
#else
    return (intptr_t)_InterlockedExchangeAdd((volatile long*)address, (long)value);
#endif
}

static inline uint8_t
_Py_atomic_add_uint8(volatile uint8_t *address, uint8_t value)
{
    return (uint8_t)_InterlockedExchangeAdd8((volatile char*)address, (char)value);
}

static inline uint32_t
_Py_atomic_add_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedExchangeAdd((volatile long*)address, (long)value);
}

static inline uint64_t
_Py_atomic_add_uint64(volatile uint64_t *address, uint64_t value)
{
    return (uint64_t)_Py_atomic_add_int64((volatile int64_t*)address, (int64_t)value);
}

static inline uintptr_t
_Py_atomic_add_uintptr(volatile uintptr_t *address, uintptr_t value)
{
#if SIZEOF_VOID_P == 8
    return (uintptr_t)_InterlockedExchangeAdd64((volatile __int64*)address, (__int64)value);
#else
    return (uintptr_t)_InterlockedExchangeAdd((volatile long*)address, (long)value);
#endif
}

static inline Py_ssize_t
_Py_atomic_add_ssize(volatile Py_ssize_t *address, Py_ssize_t value)
{
#if SIZEOF_SIZE_T == 8
    return (Py_ssize_t)_InterlockedExchangeAdd64((volatile __int64*)address, (__int64)value);
#else
    return (Py_ssize_t)_InterlockedExchangeAdd((volatile long*)address, (long)value);
#endif
}


static inline int
_Py_atomic_compare_exchange_int(volatile int *address, int expected, int value)
{
    return (long)expected == _InterlockedCompareExchange((volatile long*)address, (long)value, (long)expected);
}

static inline int
_Py_atomic_compare_exchange_int32(volatile int32_t *address, int32_t expected, int32_t value)
{
    return (long)expected == _InterlockedCompareExchange((volatile long*)address, (long)value, (long)expected);
}

static inline int
_Py_atomic_compare_exchange_int64(volatile int64_t *address, int64_t expected, int64_t value)
{
    return (__int64)expected == _InterlockedCompareExchange64((volatile __int64*)address, (__int64)value, (__int64)expected);
}

static inline int
_Py_atomic_compare_exchange_intptr(volatile intptr_t *address, intptr_t expected, intptr_t value)
{
    return (void *)expected == _InterlockedCompareExchangePointer((void * volatile *)address, (void *)value, (void *)expected);
}

static inline int
_Py_atomic_compare_exchange_uint8(volatile uint8_t *address, uint8_t expected, uint8_t value)
{
    return (char)expected == _InterlockedCompareExchange8((volatile char*)address, (char)value, (char)expected);
}

static inline int
_Py_atomic_compare_exchange_uint(volatile unsigned int *address, unsigned int expected, unsigned int value)
{
    return (long)expected == _InterlockedCompareExchange((volatile long*)address, (long)value, (long)expected);
}

static inline int
_Py_atomic_compare_exchange_uint32(volatile uint32_t *address, uint32_t expected, uint32_t value)
{
    return (long)expected == _InterlockedCompareExchange((volatile long*)address, (long)value, (long)expected);
}

static inline int
_Py_atomic_compare_exchange_uint64(volatile uint64_t *address, uint64_t expected, uint64_t value)
{
    return (__int64)expected == _InterlockedCompareExchange64((volatile __int64*)address, (__int64)value, (__int64)expected);
}

static inline int
_Py_atomic_compare_exchange_uintptr(volatile uintptr_t *address, uintptr_t expected, uintptr_t value)
{
    return (void *)expected == _InterlockedCompareExchangePointer((void * volatile *)address, (void *)value, (void *)expected);
}

static inline int
_Py_atomic_compare_exchange_ssize(volatile Py_ssize_t *address, Py_ssize_t expected, Py_ssize_t value)
{
#if SIZEOF_SIZE_T == 8
    return (__int64)expected == _InterlockedCompareExchange64((volatile __int64*)address, (__int64)value, (__int64)expected);
#else
    return (long)expected == _InterlockedCompareExchange((volatile long*)address, (long)value, (long)expected);
#endif
}

static inline int
_Py_atomic_compare_exchange_ptr(volatile void *address, void *expected, void *value)
{
    return (void *)expected == _InterlockedCompareExchangePointer((void * volatile *)address, (void *)value, (void *)expected);
}


static inline int
_Py_atomic_exchange_int(volatile int *address, int value)
{
    return (int)_InterlockedExchange((volatile long*)address, (long)value);
}

static inline int32_t
_Py_atomic_exchange_int32(volatile int32_t *address, int32_t value)
{
    return (int32_t)_InterlockedExchange((volatile long*)address, (long)value);
}

static inline int64_t
_Py_atomic_exchange_int64(volatile int64_t *address, int64_t value)
{
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (int64_t)_InterlockedExchange64((volatile __int64*)address, (__int64)value);
#else
    for (;;) {
        uint64_t old_value = *address;
        if (_InterlockedCompareExchange64((volatile __int64*)address, (__int64)value, (__int64)old_value)) {
            return old_value;
        }
    }
#endif
}

static inline intptr_t
_Py_atomic_exchange_intptr(volatile intptr_t *address, intptr_t value)
{
    return (intptr_t)_InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline uint8_t
_Py_atomic_exchange_uint8(volatile uint8_t *address, uint8_t value)
{
    return (uint8_t)_InterlockedExchange8((volatile char*)address, (char)value);
}

static inline uint32_t
_Py_atomic_exchange_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedExchange((volatile long*)address, (long)value);
}

static inline uint64_t
_Py_atomic_exchange_uint64(volatile uint64_t *address, uint64_t value)
{
    return (uint64_t)_Py_atomic_exchange_int64((volatile __int64*)address, (__int64)value);
}

static inline uintptr_t
_Py_atomic_exchange_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    return (uintptr_t)_InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline void *
_Py_atomic_exchange_ptr(volatile void *address, void *value)
{
    return (void *)_InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline uint8_t
_Py_atomic_and_uint8(volatile uint8_t *address, uint8_t value)
{
    return (uint8_t)_InterlockedAnd8((volatile char*)address, (char)value);
}

static inline uint32_t
_Py_atomic_and_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedAnd((volatile long*)address, (long)value);
}

static inline uint64_t
_Py_atomic_and_uint64(volatile uint64_t *address, uint64_t value)
{
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (uint64_t)_InterlockedAnd64((volatile __int64*)address, (__int64)value);
#else
    for (;;) {
        uint64_t old_value = *address;
        uint64_t new_value = old_value & value;
        if (_InterlockedCompareExchange64((volatile __int64*)address, (__int64)new_value, (__int64)old_value)) {
            return old_value;
        }
    }
#endif
}

static inline uintptr_t
_Py_atomic_and_uintptr(volatile uintptr_t *address, uintptr_t value)
{
#if SIZEOF_VOID_P == 8
    return (uintptr_t)_InterlockedAnd64((volatile __int64*)address, (__int64)value);
#else
    return (uintptr_t)_InterlockedAnd((volatile long*)address, (long)value);
#endif
}

static inline uint8_t
_Py_atomic_or_uint8(volatile uint8_t *address, uint8_t value)
{
    return (uint8_t)_InterlockedOr8((volatile char*)address, (char)value);
}

static inline uint32_t
_Py_atomic_or_uint32(volatile uint32_t *address, uint32_t value)
{
    return (uint32_t)_InterlockedOr((volatile long*)address, (long)value);
}

static inline uint64_t
_Py_atomic_or_uint64(volatile uint64_t *address, uint64_t value)
{
#if defined(_M_X64) || defined(_M_ARM64) || defined(_M_HYBRID_X86_ARM64) || defined(_M_ARM64EC)
    return (uint64_t)_InterlockedOr64((volatile __int64*)address, (__int64)value);
#else
    for (;;) {
        uint64_t old_value = *address;
        uint64_t new_value = old_value | value;
        if (_InterlockedCompareExchange64((volatile __int64*)address, (__int64)new_value, (__int64)old_value)) {
            return old_value;
        }
    }
#endif
}

static inline uintptr_t
_Py_atomic_or_uintptr(volatile uintptr_t *address, uintptr_t value)
{
#if SIZEOF_VOID_P == 8
    return (uintptr_t)_InterlockedOr64((volatile __int64*)address, (__int64)value);
#else
    return (uintptr_t)_InterlockedOr((volatile long*)address, (long)value);
#endif
}

static inline int
_Py_atomic_load_int(const volatile int *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return (int)__ldar32((unsigned __int32 volatile*)address);
#else
#error no implementation of _Py_atomic_load_int
#endif
}

static inline int32_t
_Py_atomic_load_int32(const volatile int32_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return (int)__ldar32((unsigned __int32 volatile*)address);
#else
#error no implementation of _Py_atomic_load_int32
#endif
}

static inline int64_t
_Py_atomic_load_int64(const volatile int64_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_int64
#endif
}

static inline intptr_t
_Py_atomic_load_intptr(const volatile intptr_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_intptr
#endif
}

static inline uint8_t
_Py_atomic_load_uint8(const volatile uint8_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar8((unsigned __int8 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uint8
#endif
}

static inline uint16_t
_Py_atomic_load_uint16(const volatile uint16_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar16((unsigned __int16 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uint16
#endif
}

static inline uint32_t
_Py_atomic_load_uint32(const volatile uint32_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar32((unsigned __int32 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uint32
#endif
}

static inline uint64_t
_Py_atomic_load_uint64(const volatile uint64_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uint64
#endif
}

static inline uintptr_t
_Py_atomic_load_uintptr(const volatile uintptr_t *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uintptr
#endif
}

static inline unsigned int
_Py_atomic_load_uint(const volatile unsigned int *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar32((unsigned __int32 volatile*)address);
#else
#error no implementation of _Py_atomic_load_uint
#endif
}

static inline unsigned long
_Py_atomic_load_ulong(const volatile unsigned long *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar32((unsigned __int32 volatile*)address);
#else
#error no implementation of _Py_atomic_load_ulong
#endif
}

static inline Py_ssize_t
_Py_atomic_load_ssize(const volatile Py_ssize_t* address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *address;
#elif defined(_M_ARM64)
    return __ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_ssize
#endif
}

static inline void *
_Py_atomic_load_ptr(const volatile void *address)
{
#if defined(_M_X64) || defined(_M_IX86)
    return *(void* volatile*)address;
#elif defined(_M_ARM64)
    return (void *)__ldar64((unsigned __int64 volatile*)address);
#else
#error no implementation of _Py_atomic_load_ptr
#endif
}

static inline int
_Py_atomic_load_int_relaxed(const volatile int* address)
{
    return *address;
}

static inline int8_t
_Py_atomic_load_int8_relaxed(const volatile int8_t* address)
{
    return *address;
}

static inline int16_t
_Py_atomic_load_int16_relaxed(const volatile int16_t* address)
{
    return *address;
}

static inline int32_t
_Py_atomic_load_int32_relaxed(const volatile int32_t* address)
{
    return *address;
}

static inline int64_t
_Py_atomic_load_int64_relaxed(const volatile int64_t* address)
{
    return *address;
}

static inline intptr_t
_Py_atomic_load_intptr_relaxed(const volatile intptr_t* address)
{
    return *address;
}

static inline uint8_t
_Py_atomic_load_uint8_relaxed(const volatile uint8_t* address)
{
    return *address;
}

static inline uint16_t
_Py_atomic_load_uint16_relaxed(const volatile uint16_t* address)
{
    return *address;
}

static inline uint32_t
_Py_atomic_load_uint32_relaxed(const volatile uint32_t* address)
{
    return *address;
}

static inline uint64_t
_Py_atomic_load_uint64_relaxed(const volatile uint64_t* address)
{
    return *address;
}

static inline uintptr_t
_Py_atomic_load_uintptr_relaxed(const volatile uintptr_t* address)
{
    return *address;
}

static inline unsigned int
_Py_atomic_load_uint_relaxed(const volatile unsigned int *address)
{
    return *address;
}

static inline unsigned long
_Py_atomic_load_ulong_relaxed(const volatile unsigned long *address)
{
    return *address;
}

static inline Py_ssize_t
_Py_atomic_load_ssize_relaxed(const volatile Py_ssize_t* address)
{
    return *address;
}

static inline void*
_Py_atomic_load_ptr_relaxed(const volatile void* address)
{
    return *(void * volatile *)address;
}



static inline void
_Py_atomic_store_int(volatile int *address, int value)
{
    _InterlockedExchange((volatile long*)address, (long)value);
}

static inline void
_Py_atomic_store_int32(volatile int32_t *address, int32_t value)
{
    _InterlockedExchange((volatile long*)address, (long)value);
}

static inline void
_Py_atomic_store_int64(volatile int64_t *address, int64_t value)
{
    _Py_atomic_exchange_int64(address, value);
}

static inline void
_Py_atomic_store_intptr(volatile intptr_t *address, intptr_t value)
{
    _InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline void
_Py_atomic_store_uint8(volatile uint8_t *address, uint8_t value)
{
    _InterlockedExchange8((volatile char*)address, (char)value);
}

static inline void
_Py_atomic_store_uint16(volatile uint16_t *address, uint16_t value)
{
    _InterlockedExchange16((volatile short*)address, (short)value);
}

static inline void
_Py_atomic_store_uint32(volatile uint32_t *address, uint32_t value)
{
    _InterlockedExchange((volatile long*)address, (long)value);
}

static inline void
_Py_atomic_store_uint64(volatile uint64_t *address, uint64_t value)
{
    _Py_atomic_exchange_int64((volatile __int64*)address, (__int64)value);
}

static inline void
_Py_atomic_store_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    _InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline void
_Py_atomic_store_uint(volatile unsigned int *address, unsigned int value)
{
    _InterlockedExchange((volatile long*)address, (long)value);
}

static inline void
_Py_atomic_store_ulong(volatile unsigned long *address, unsigned long value)
{
    _InterlockedExchange((volatile long*)address, (long)value);
}

static inline void
_Py_atomic_store_ptr(volatile void *address, void *value)
{
    _InterlockedExchangePointer((void * volatile *)address, (void *)value);
}

static inline void
_Py_atomic_store_ssize(volatile Py_ssize_t* address, Py_ssize_t value)
{
#if SIZEOF_SIZE_T == 8
    _InterlockedExchange64((volatile __int64*)address, (__int64)value);
#else
    _InterlockedExchange((volatile long*)address, (long)value);
#endif
}


static inline void
_Py_atomic_store_int_relaxed(volatile int* address, int value)
{
    *address = value;
}

static inline void
_Py_atomic_store_int8_relaxed(volatile int8_t* address, int8_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_int16_relaxed(volatile int16_t* address, int16_t value)
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
_Py_atomic_store_uint8_relaxed(volatile uint8_t* address, uint8_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uint16_relaxed(volatile uint16_t* address, uint16_t value)
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
_Py_atomic_store_uintptr_relaxed(volatile uintptr_t* address, uintptr_t value)
{
    *address = value;
}

static inline void
_Py_atomic_store_uint_relaxed(volatile unsigned int *address, unsigned int value)
{
    *address = value;
}

static inline void
_Py_atomic_store_ulong_relaxed(volatile unsigned long *address, unsigned long value)
{
    *address = value;
}

static inline void
_Py_atomic_store_ptr_relaxed(volatile void* address, void* value)
{
    *(void * volatile *)address = value;
}

static inline void
_Py_atomic_store_ssize_relaxed(volatile Py_ssize_t* address, Py_ssize_t value)
{
    *address = value;
}


static inline void
_Py_atomic_store_uint64_release(volatile uint64_t* address, uint64_t value)
{
#if defined(_M_X64) || defined(_M_IX86)
    *address = value;
#elif defined(_M_ARM64)
    __stlr64(address, value);
#else
#error no implementation of _Py_atomic_store_uint64_release
#endif
}

static inline void
_Py_atomic_store_ptr_release(volatile void* address, void* value)
{
#if defined(_M_X64) || defined(_M_IX86)
    *(void * volatile *)address = value;
#elif defined(_M_ARM64)
    __stlr64(address, (uintptr_t)value);
#else
#error no implementation of _Py_atomic_store_ptr_release
#endif
}

 static inline void
_Py_atomic_fence_seq_cst(void)
{
#if defined(_M_ARM64) || defined(_M_ARM)
    __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_X64)
    __faststorefence();
#elif defined(_M_IX86)
    _mm_mfence();
#else
#error no implementation of _Py_atomic_fence_seq_cst
#endif
}

 static inline void
_Py_atomic_fence_release(void)
{
#if defined(_M_ARM64) || defined(_M_ARM)
    __dmb(_ARM64_BARRIER_ISH);
#elif defined(_M_X64) || defined(_M_IX86)
    _ReadWriteBarrier();
#else
#error no implementation of _Py_atomic_fence_release
#endif
}
