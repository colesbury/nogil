#ifndef Py_INTERNAL_DICT_H
#define Py_INTERNAL_DICT_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include <x86intrin.h>

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


static inline int
ctrl_has_empty(__m128i ctrl)
{
    return _mm_movemask_epi8(_mm_cmpeq_epi8(ctrl, _mm_set1_epi8(CTRL_EMPTY)));
}

__attribute__((no_sanitize("thread")))
static inline __m128i
_mm_loadu_si128_nonatomic(void *p)
{
    return _mm_loadu_si128((__m128i *)p);
}

static inline PyDictKeyEntry *
find_unicode(PyDictKeysObject *keys, PyObject *key)
{
    PyDictKeyEntry *entries = keys->dk_entries;
    size_t mask = keys->dk_size & ~15;
    Py_hash_t hash = ((PyASCIIObject *)key)->hash;
    Py_hash_t ix = (hash >> 7) & mask;
    __m128i match = _mm_set1_epi8(CTRL_FULL | (hash & 0x7F));
    for (;;) {
        __m128i ctrl = _mm_loadu_si128_nonatomic(keys->dk_ctrl + ix);
        int bitmask = _mm_movemask_epi8(_mm_cmpeq_epi8(match, ctrl));
        while (bitmask) {
            int lsb = __builtin_ctz(bitmask);
            PyDictKeyEntry *entry = &entries[ix + lsb];
            if (_PY_LIKELY(entry->me_key == key)) {
                return entry;
            }
            bitmask &= bitmask - 1;
        }
        if (_PY_LIKELY(ctrl_has_empty(ctrl))) {
            return NULL;
        }
        ix = (ix + 16) & mask;
    }
}

static inline int
dict_may_contain(PyDictObject *dict, PyObject *key)
{
    PyDictKeysObject *keys = _Py_atomic_load_ptr_relaxed(&dict->ma_keys);
    if (keys->dk_type != DK_UNICODE) {
        return 1;
    }
    size_t mask = keys->dk_size & ~15;
    Py_hash_t hash = ((PyASCIIObject *)key)->hash;
    Py_hash_t ix = (hash >> 7) & mask;
    __m128i match = _mm_set1_epi8(CTRL_FULL | (hash & 0x7F));
    __m128i ctrl = _mm_loadu_si128_nonatomic(keys->dk_ctrl + ix);
    if (!ctrl_has_empty(ctrl)) { 
        return 1;
    }
    int bitmask = _mm_movemask_epi8(_mm_cmpeq_epi8(match, ctrl));
    return bitmask != 0;
}

#endif /* !Py_INTERNAL_DICT_H */
