#ifndef Py_ATOMIC_H
#define Py_ATOMIC_H

static inline int
_Py_atomic_add_int(volatile int *address, int value);

static inline int32_t
_Py_atomic_add_int32(volatile int32_t *address, int32_t value);

static inline int64_t
_Py_atomic_add_int64(volatile int64_t *address, int64_t value);

static inline intptr_t
_Py_atomic_add_intptr(volatile intptr_t *address, intptr_t value);

static inline uint32_t
_Py_atomic_add_uint32(volatile uint32_t *address, uint32_t value);

static inline uint64_t
_Py_atomic_add_uint64(volatile uint64_t *address, uint64_t value);

static inline uintptr_t
_Py_atomic_add_uintptr(volatile uintptr_t *address, uintptr_t value);

static inline Py_ssize_t
_Py_atomic_add_ssize(volatile Py_ssize_t *address, Py_ssize_t value);


static inline int
_Py_atomic_compare_exchange_int(volatile int *address, int expected, int value);

static inline int
_Py_atomic_compare_exchange_int32(volatile int32_t *address, int32_t expected, int32_t value);

static inline int
_Py_atomic_compare_exchange_int64(volatile int64_t *address, int64_t expected, int64_t value);

static inline int
_Py_atomic_compare_exchange_intptr(volatile intptr_t *address, intptr_t expected, intptr_t value);

static inline int
_Py_atomic_compare_exchange_uint32(volatile uint32_t *address, uint32_t expected, uint32_t value);

static inline int
_Py_atomic_compare_exchange_uint64(volatile uint64_t *address, uint64_t expected, uint64_t value);

static inline int
_Py_atomic_compare_exchange_uintptr(volatile uintptr_t *address, uintptr_t expected, uintptr_t value);

static inline int
_Py_atomic_compare_exchange_ptr(volatile void *address, void *expected, void *value);


static inline int
_Py_atomic_exchange_int(volatile int *address, int value);

static inline int32_t
_Py_atomic_exchange_int32(volatile int32_t *address, int32_t value);

static inline int64_t
_Py_atomic_exchange_int64(volatile int64_t *address, int64_t value);

static inline intptr_t
_Py_atomic_exchange_intptr(volatile intptr_t *address, intptr_t value);

static inline uint32_t
_Py_atomic_exchange_uint32(volatile uint32_t *address, uint32_t value);

static inline uint64_t
_Py_atomic_exchange_uint64(volatile uint64_t *address, uint64_t value);

static inline uintptr_t
_Py_atomic_exchange_uintptr(volatile uintptr_t *address, uintptr_t value);

static inline void *
_Py_atomic_exchange_ptr(volatile void *address, void *value);


static inline int
_Py_atomic_load_int(const volatile int *address);

static inline int32_t
_Py_atomic_load_int32(const volatile int32_t *address);

static inline int64_t
_Py_atomic_load_int64(const volatile int64_t *address);

static inline intptr_t
_Py_atomic_load_intptr(const volatile intptr_t *address);

static inline uint32_t
_Py_atomic_load_uint32(const volatile uint32_t *address);

static inline uint64_t
_Py_atomic_load_uint64(const volatile uint64_t *address);

static inline uintptr_t
_Py_atomic_load_uintptr(const volatile uintptr_t *address);

static inline Py_ssize_t
_Py_atomic_load_ssize(const volatile Py_ssize_t *address);

static inline void *
_Py_atomic_load_ptr(const volatile void *address);


static inline int
_Py_atomic_load_int_relaxed(const volatile int *address);

static inline int32_t
_Py_atomic_load_int32_relaxed(const volatile int32_t *address);

static inline int64_t
_Py_atomic_load_int64_relaxed(const volatile int64_t *address);

static inline intptr_t
_Py_atomic_load_intptr_relaxed(const volatile intptr_t *address);

static inline uint32_t
_Py_atomic_load_uint32_relaxed(const volatile uint32_t *address);

static inline uint64_t
_Py_atomic_load_uint64_relaxed(const volatile uint64_t *address);

static inline uintptr_t
_Py_atomic_load_uintptr_relaxed(const volatile uintptr_t *address);

static inline Py_ssize_t
_Py_atomic_load_ssize_relaxed(const volatile Py_ssize_t *address);

static inline void *
_Py_atomic_load_ptr_relaxed(const volatile void *address);


static inline void
_Py_atomic_store_int(volatile int *address, int value);

static inline void
_Py_atomic_store_int32(volatile int32_t *address, int32_t value);

static inline void
_Py_atomic_store_int64(volatile int64_t *address, int64_t value);

static inline void
_Py_atomic_store_intptr(volatile intptr_t *address, intptr_t value);

static inline void
_Py_atomic_store_uint32(volatile uint32_t *address, uint32_t value);

static inline void
_Py_atomic_store_uint64(volatile uint64_t *address, uint64_t value);

static inline void
_Py_atomic_store_uintptr(volatile uintptr_t *address, uintptr_t value);

static inline void
_Py_atomic_store_ptr(volatile void *address, void *value);

static inline void
_Py_atomic_store_ssize(volatile Py_ssize_t* address, Py_ssize_t value);


static inline void
_Py_atomic_store_int_relaxed(volatile int *address, int value);

static inline void
_Py_atomic_store_int32_relaxed(volatile int32_t *address, int32_t value);

static inline void
_Py_atomic_store_int64_relaxed(volatile int64_t *address, int64_t value);

static inline void
_Py_atomic_store_intptr_relaxed(volatile intptr_t *address, intptr_t value);

static inline void
_Py_atomic_store_uint32_relaxed(volatile uint32_t *address, uint32_t value);

static inline void
_Py_atomic_store_uint64_relaxed(volatile uint64_t *address, uint64_t value);

static inline void
_Py_atomic_store_uint64_release(volatile uint64_t *address, uint64_t value);

static inline void
_Py_atomic_store_uintptr_relaxed(volatile uintptr_t *address, uintptr_t value);

static inline void
_Py_atomic_store_ptr_relaxed(volatile void *address, void *value);

static inline void
_Py_atomic_store_ptr_release(volatile void *address, void *value);

static inline void
_Py_atomic_store_ssize_relaxed(volatile Py_ssize_t *address, Py_ssize_t value);

static inline void
_Py_atomic_store_uint8_relaxed(volatile uint8_t* address, uint8_t value);

 static inline void
_Py_atomic_fence_seq_cst(void);

 static inline void
_Py_atomic_fence_release(void);

static inline int
_Py_atomic_uintptr_is_zero(uintptr_t *address)
{
#if defined(__GNUC__) && defined(__GCC_ASM_FLAG_OUTPUTS__) && defined(__x86_64__)
    int out;
    __asm__ (
        "cmpq\t$0, %[address]"
        : "=@ccz" (out)
        : [address] "m" (*address)
        : );
    return out;
#else
    return _Py_atomic_load_uintptr_relaxed(address) == 0;
#endif
}

static inline int
_Py_atomic_compare_uintptr_relaxed(uintptr_t *address, uintptr_t value)
{
    // GCC and clang generate an unecessary `mov` instead of using
    // `cmp` with a memory operand. The inline assembly avoids this.
#if defined(__GNUC__) && defined(__GCC_ASM_FLAG_OUTPUTS__) && defined(__x86_64__)
    int out;
    __asm__ (
        "cmp    %[value], %[address]"
        : "=@ccz" (out)
        : [value] "r"(value), [address] "m" (*address)
        : );
    return out;
#else
    return _Py_atomic_load_uintptr_relaxed(address) == value;
#endif
}


#if defined(HAVE_STD_ATOMIC) || defined(HAVE_BUILTIN_ATOMIC)
#define Py_ATOMIC_STD_H
#include "pyatomic_std.h"
#elif defined(_MSC_VER)
#define Py_ATOMIC_MSC_H
#include "pyatomic_msc.h"
#else
#error "define pyatomic for this platform"
#endif

#endif  /* Py_ATOMIC_H */
