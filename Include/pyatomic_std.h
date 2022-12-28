#ifndef Py_ATOMIC_STD_H
#  error "this header file must not be included directly"
#endif

#ifdef __cplusplus
extern "C++" {
#include <atomic>
}
#define  MI_USING_STD   using namespace std;
#define  _Atomic(tp)    atomic<tp>
#else
#include <stdatomic.h>
#define  MI_USING_STD
#endif


static inline int
_Py_atomic_add_int(volatile int *address, int value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(int)*)address, value);
}

static inline int32_t
_Py_atomic_add_int32(volatile int32_t *address, int32_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(int32_t)*)address, value);
}

static inline int64_t
_Py_atomic_add_int64(volatile int64_t *address, int64_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(int64_t)*)address, value);
}

static inline intptr_t
_Py_atomic_add_intptr(volatile intptr_t *address, intptr_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(intptr_t)*)address, value);
}

static inline uint32_t
_Py_atomic_add_uint32(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(uint32_t)*)address, value);
}

static inline uint64_t
_Py_atomic_add_uint64(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(uint64_t)*)address, value);
}

static inline uintptr_t
_Py_atomic_add_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(uintptr_t)*)address, value);
}

static inline Py_ssize_t
_Py_atomic_add_ssize(volatile Py_ssize_t *address, Py_ssize_t value)
{
    MI_USING_STD
    return atomic_fetch_add((volatile _Atomic(Py_ssize_t)*)address, value);
}


static inline int
_Py_atomic_compare_exchange_int(volatile int *address, int expected, int value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(int)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_int32(volatile int32_t *address, int32_t expected, int32_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(int32_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_int64(volatile int64_t *address, int64_t expected, int64_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(int64_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_intptr(volatile intptr_t *address, intptr_t expected, intptr_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(intptr_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_uint32(volatile uint32_t *address, uint32_t expected, uint32_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(uint32_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_uint64(volatile uint64_t *address, uint64_t expected, uint64_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(uint64_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_uintptr(volatile uintptr_t *address, uintptr_t expected, uintptr_t value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(uintptr_t)*)address, &expected, value);
}

static inline int
_Py_atomic_compare_exchange_ptr(volatile void *address, void *expected, void *value)
{
    MI_USING_STD
    return atomic_compare_exchange_strong((volatile _Atomic(void *)*)address, &expected, value);
}


static inline int
_Py_atomic_exchange_int(volatile int *address, int value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(int)*)address, value);
}

static inline int32_t
_Py_atomic_exchange_int32(volatile int32_t *address, int32_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(int32_t)*)address, value);
}

static inline int64_t
_Py_atomic_exchange_int64(volatile int64_t *address, int64_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(int64_t)*)address, value);
}

static inline intptr_t
_Py_atomic_exchange_intptr(volatile intptr_t *address, intptr_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(intptr_t)*)address, value);
}

static inline uint32_t
_Py_atomic_exchange_uint32(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(uint32_t)*)address, value);
}

static inline uint64_t
_Py_atomic_exchange_uint64(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(uint64_t)*)address, value);
}

static inline uintptr_t
_Py_atomic_exchange_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(uintptr_t)*)address, value);
}

static inline void *
_Py_atomic_exchange_ptr(volatile void *address, void *value)
{
    MI_USING_STD
    return atomic_exchange((volatile _Atomic(void *)*)address, value);
}

static inline uint32_t
_Py_atomic_and_uint32(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    return atomic_and((volatile _Atomic(uint32_t)*)address, value);
}

static inline uint64_t
_Py_atomic_and_uint64(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    return atomic_and((volatile _Atomic(uint64_t)*)address, value);
}

static inline uintptr_t
_Py_atomic_and_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    return atomic_and((volatile _Atomic(uintptr_t)*)address, value);
}

static inline uint32_t
_Py_atomic_or_uint32(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    return atomic_or((volatile _Atomic(uint32_t)*)address, value);
}

static inline uint64_t
_Py_atomic_or_uint64(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    return atomic_or((volatile _Atomic(uint64_t)*)address, value);
}

static inline uintptr_t
_Py_atomic_or_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    return atomic_or((volatile _Atomic(uintptr_t)*)address, value);
}

static inline int
_Py_atomic_load_int(const volatile int *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(int)*)address);
}

static inline int32_t
_Py_atomic_load_int32(const volatile int32_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(int32_t)*)address);
}

static inline int64_t
_Py_atomic_load_int64(const volatile int64_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(int64_t)*)address);
}

static inline intptr_t
_Py_atomic_load_intptr(const volatile intptr_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(intptr_t)*)address);
}

static inline uint32_t
_Py_atomic_load_uint32(const volatile uint32_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(uint32_t)*)address);
}

static inline uint64_t
_Py_atomic_load_uint64(const volatile uint64_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(uint64_t)*)address);
}

static inline uintptr_t
_Py_atomic_load_uintptr(const volatile uintptr_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(uintptr_t)*)address);
}

static inline Py_ssize_t
_Py_atomic_load_ssize(const volatile Py_ssize_t *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(Py_ssize_t)*)address);
}

static inline void *
_Py_atomic_load_ptr(const volatile void *address)
{
    MI_USING_STD
    return atomic_load((const volatile _Atomic(void*)*)address);
}


static inline int
_Py_atomic_load_int_relaxed(const volatile int *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(int)*)address, memory_order_relaxed);
}

static inline int32_t
_Py_atomic_load_int32_relaxed(const volatile int32_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(int32_t)*)address, memory_order_relaxed);
}

static inline int64_t
_Py_atomic_load_int64_relaxed(const volatile int64_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(int64_t)*)address, memory_order_relaxed);
}

static inline intptr_t
_Py_atomic_load_intptr_relaxed(const volatile intptr_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(intptr_t)*)address, memory_order_relaxed);
}

static inline uint32_t
_Py_atomic_load_uint32_relaxed(const volatile uint32_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(uint32_t)*)address, memory_order_relaxed);
}

static inline uint64_t
_Py_atomic_load_uint64_relaxed(const volatile uint64_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(uint64_t)*)address, memory_order_relaxed);
}

static inline uintptr_t
_Py_atomic_load_uintptr_relaxed(const volatile uintptr_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(uintptr_t)*)address, memory_order_relaxed);
}

static inline Py_ssize_t
_Py_atomic_load_ssize_relaxed(const volatile Py_ssize_t *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(Py_ssize_t)*)address, memory_order_relaxed);
}

static inline void *
_Py_atomic_load_ptr_relaxed(const volatile void *address)
{
    MI_USING_STD
    return atomic_load_explicit((const volatile _Atomic(void*)*)address, memory_order_relaxed);
}

static inline void
_Py_atomic_store_int(volatile int *address, int value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(int)*)address, value);
}

static inline void
_Py_atomic_store_int32(volatile int32_t *address, int32_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(int32_t)*)address, value);
}

static inline void
_Py_atomic_store_int64(volatile int64_t *address, int64_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(int64_t)*)address, value);
}

static inline void
_Py_atomic_store_intptr(volatile intptr_t *address, intptr_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(intptr_t)*)address, value);
}

static inline void
_Py_atomic_store_uint32(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(uint32_t)*)address, value);
}

static inline void
_Py_atomic_store_uint64(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(uint64_t)*)address, value);
}

static inline void
_Py_atomic_store_uintptr(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(uintptr_t)*)address, value);
}

static inline void
_Py_atomic_store_ptr(volatile void *address, void *value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(void*)*)address, value);
}

static inline void
_Py_atomic_store_ssize(volatile Py_ssize_t *address, Py_ssize_t value)
{
    MI_USING_STD
    atomic_store((volatile _Atomic(Py_ssize_t)*)address, value);
}

static inline void
_Py_atomic_store_int_relaxed(volatile int *address, int value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(int)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_int32_relaxed(volatile int32_t *address, int32_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(int32_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_int64_relaxed(volatile int64_t *address, int64_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(int64_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_intptr_relaxed(volatile intptr_t *address, intptr_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(intptr_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_uint32_relaxed(volatile uint32_t *address, uint32_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(uint32_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_uint64_relaxed(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(uint64_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_uint64_release(volatile uint64_t *address, uint64_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(uint64_t)*)address, value, memory_order_release);
}

static inline void
_Py_atomic_store_uintptr_relaxed(volatile uintptr_t *address, uintptr_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(uintptr_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_ptr_relaxed(volatile void *address, void *value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(void*)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_ptr_release(volatile void *address, void *value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(void*)*)address, value, memory_order_release);
}

static inline void
_Py_atomic_store_ssize_relaxed(volatile Py_ssize_t *address, Py_ssize_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(Py_ssize_t)*)address, value, memory_order_relaxed);
}

static inline void
_Py_atomic_store_uint8_relaxed(volatile uint8_t *address, uint8_t value)
{
    MI_USING_STD
    atomic_store_explicit((volatile _Atomic(uint8_t)*)address, value, memory_order_relaxed);
}

 static inline void
_Py_atomic_fence_seq_cst(void)
{
    MI_USING_STD
    atomic_thread_fence(memory_order_seq_cst);
}

 static inline void
_Py_atomic_fence_release(void)
{
    MI_USING_STD
    atomic_thread_fence(memory_order_release);
}
