#ifndef Py_INTERNAL_DICT_H
#define Py_INTERNAL_DICT_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif HAVE_SSE2
#include <x86intrin.h>
#elif HAVE_NEON
#include <arm_neon.h>
#endif

#if HAVE_SSE2 || HAVE_NEON
#define DICT_GROUP_SIZE 16
#else
#define DICT_GROUP_SIZE 16
#endif

#if HAVE_SSE2
typedef int         dict_bitmask;
typedef __m128i     dict_ctrl;
#elif HAVE_NEON
typedef uint64_t    dict_bitmask;
typedef uint8x16_t  dict_ctrl;
#else
typedef uint64_t    dict_bitmask;
typedef uint64_t    dict_ctrl;
#endif

#define DICT_SIZE_MASK (~(DICT_GROUP_SIZE - 1))

typedef struct {
    PyObject *me_key;
    PyObject *me_value; /* This field is only meaningful for combined tables */
} PyDictKeyEntry;

enum {
    CTRL_EMPTY = 0,
    CTRL_DELETED = 1,
    CTRL_FULL = 0x80
};

enum {
    DK_UNICODE = 1,
    DK_GENERIC = 4
};

// See dictobject.c for actual layout of DictKeysObject
struct _dictkeysobject {
    // Number of usable entries in dk_entries
    // Note: this field is clobbered when the object is freed
    Py_ssize_t dk_usable;

    // Hashtable type (DK_UNICODE, DK_SPLIT, or DK_GENERIC)
    uint8_t dk_type;

    Py_ssize_t dk_size;

    PyDictKeyEntry *dk_entries;

    /* Number of used entries in dk_entries. */
    Py_ssize_t dk_nentries;

    uint8_t dk_ctrl[];

    //
    // Py_hash_t dk_hashes[dk_size]; (optional)
    //
    // PyDictKeyEntry dk_entries[dk_size];
    //
    // <varies> dk_indices[dk_usable + 1];
    //
};

static inline dict_bitmask
ctrl_match_empty(dict_ctrl ctrl)
{
#ifdef HAVE_SSE2
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_setzero_si128(), ctrl));
#elif HAVE_NEON
    uint8x16_t test = vtstq_u8(ctrl, ctrl);
    uint8x8_t maskV = vshrn_n_u16(vreinterpretq_u16_u8(test), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(maskV), 0);
    return ~mask;
#else
    uint64_t msbs = 0x8080808080808080ULL;
    uint64_t x = ~ctrl;
    return (x & (x << 7)) & msbs;
#endif
}

static inline int
ctrl_has_empty(dict_ctrl ctrl)
{
    return ctrl_match_empty(ctrl) != 0;
}

static inline int
ctrl_is_full(uint8_t ctrl)
{
    return (ctrl & CTRL_FULL) != 0;
}

static inline dict_ctrl _Py_NO_SANITIZE_THREAD
load_ctrl(PyDictKeysObject *keys, Py_ssize_t ix)
{
#ifdef HAVE_SSE2
    return _mm_loadu_si128((dict_ctrl *)(keys->dk_ctrl + ix));
#elif HAVE_NEON
    return vld1q_u8((uint8_t *)(keys->dk_ctrl + ix));
#else
    return _Py_atomic_load_uint64_relaxed((dict_ctrl *)(keys->dk_ctrl + ix));
#endif
}

static inline dict_bitmask
dict_match(dict_ctrl ctrl, Py_ssize_t hash)
{
#ifdef HAVE_SSE2
    dict_ctrl needle = _mm_set1_epi8(CTRL_FULL | (hash & 0x7F));
    return _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, needle));
#elif HAVE_NEON
    uint64_t lsbs = 0x1111111111111111ULL;
    dict_ctrl needle = vdupq_n_u8(CTRL_FULL | (hash & 0x7F));
    dict_ctrl eq = vceqq_u8(ctrl, needle);
    uint8x8_t maskV = vshrn_n_u16(vreinterpretq_u16_u8(eq), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(maskV), 0);
    return mask & lsbs;
#else
    uint64_t msbs = 0x8080808080808080ULL;
    uint64_t lsbs = 0x0101010101010101ULL;
    uint64_t needle = lsbs * (CTRL_FULL | (hash & 0x7F));
    uint64_t match = ctrl ^ needle;
    return (match - lsbs) & ~match & msbs & needle;
#endif
}

static inline int
bitmask_lsb(dict_bitmask bitmask)
{
#ifdef HAVE_SSE2
#if defined(_MSC_VER)
    unsigned long ret;
    _BitScanForward(&ret, bitmask);
    return (int)ret;
#else
    return __builtin_ctz(bitmask);
#endif
#elif HAVE_NEON
    return __builtin_ctzll(bitmask) >> 2;
#else
    return __builtin_ctzll(bitmask) >> 3;
#endif
}

static inline uint64_t
_PyDict_VersionTag(PyObject *mp)
{
    PyDictObject *dict = (PyDictObject *)mp;
    return _Py_atomic_load_uint64(&dict->ma_version_tag);
}

static inline PyDictKeyEntry *
find_unicode(PyDictKeysObject *keys, PyObject *key)
{
    assert(PyUnicode_CheckExact(key) && keys->dk_type == DK_UNICODE);
    PyDictKeyEntry *entries = keys->dk_entries;
    size_t mask = keys->dk_size & DICT_SIZE_MASK;
    Py_hash_t hash = ((PyASCIIObject *)key)->hash;
    Py_hash_t ix = (hash >> 7) & mask;
    for (;;) {
        dict_ctrl ctrl = load_ctrl(keys, ix);
        dict_bitmask bitmask = dict_match(ctrl, hash);
        while (bitmask) {
            int lsb = bitmask_lsb(bitmask);
            PyDictKeyEntry *entry = &entries[ix + lsb];
            if (_PY_LIKELY(entry->me_key == key)) {
                return entry;
            }
            bitmask &= bitmask - 1;
        }
        if (_PY_LIKELY(ctrl_has_empty(ctrl))) {
            return NULL;
        }
        ix = (ix + DICT_GROUP_SIZE) & mask;
    }
}

#endif /* !Py_INTERNAL_DICT_H */
