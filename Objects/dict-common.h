#ifndef Py_DICT_COMMON_H
#define Py_DICT_COMMON_H

#include "lock.h"

typedef struct {
    /* Cached hash code of me_key. */
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value; /* This field is only meaningful for combined tables */
} PyDictKeyEntry;


#define DKIX_EMPTY (-1)
#define DKIX_DUMMY (-2)  /* Used internally */
#define DKIX_ERROR (-3)

#define DK_SIZE(dk) (1LL << (dk)->dk_size_shift)

enum {
  DK_UNICODE = 1,
  DK_SPLIT = 3,
  DK_GENERIC = 4
};

// See dictobject.c for actual layout of DictKeysObject
struct _dictkeysobject {
    // Log2 of the size of the hash table (dk_indices). Minimum value is 3
    // for 8 hash table entries and 5 usable values. Maximum value depends on
    // available memory; must be less than 64 on current 64-bit systems.
    uint8_t dk_size_shift;

    // Size in bytes of dk_indices (e.g. 1, 2, 4, or 8) 
    uint8_t dk_ix_size;

    // Hashtable type (DK_UNICODE, DK_SPLIT, or DK_GENERIC)
    uint8_t dk_type;

    // Unused???
    uint8_t dk_prototype;

    /* Number of usable entries in dk_entries. */
    Py_ssize_t dk_usable;

    /* Number of used entries in dk_entries. */
    Py_ssize_t dk_nentries;

    // Actual hash table of (1<<dk_size_shift) entries. It holds indices in dk_entries,
    // or DKIX_EMPTY(-1) or DKIX_DUMMY(-2).
    //
    // Indices must be: 0 <= indice < USABLE_FRACTION(dk_size).
    //
    // The size in bytes of an indice depends on dk_size:
    //
    // - 1 byte if dk_size <= 0xff (char*)
    // - 2 bytes if dk_size <= 0xffff (int16_t*)
    // - 4 bytes if dk_size <= 0xffffffff (int32_t*)
    // - 8 bytes otherwise (int64_t*)
    //
    // Dynamically sized, SIZEOF_VOID_P is minimum. */
    char dk_indices[];  /* char is required to avoid strict aliasing. */

    /* "PyDictKeyEntry dk_entries[dk_usable];" array follows:
       see the DK_ENTRIES() macro */
};

#endif
