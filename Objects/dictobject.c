/* PyDict_MINSIZE is the starting size for any new dict.
 * 8 allows dicts with no more than 5 active entries; experiments suggested
 * this suffices for the majority of dicts (consisting mostly of usually-small
 * dicts created to pass keyword arguments).
 * Making this 8, rather than 4 reduces the number of resizes for most
 * dictionaries, without any significant extra memory use.
 */
#define PyDict_MINSIZE 7

#include "Python.h"
#include "pycore_critical_section.h"
#include "pycore_gc.h"       // _PyObject_GC_IS_TRACKED()
#include "pycore_object.h"
#include "pycore_pystate.h"  // _PyThreadState_GET()
#include "pycore_dict.h"
#include "ceval_meta.h"
#include "lock.h"
#include "stringlib/eq.h"    // unicode_eq()

#include "mimalloc.h"
#include "mimalloc-internal.h"

/*[clinic input]
class dict "PyDictObject *" "&PyDict_Type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=f157a5a0ce9589d6]*/


/* forward declarations */
static int resize(PyDictObject *mp, Py_ssize_t size, uint8_t type);
static int reserve(PyDictObject *mp, Py_ssize_t usable);
static Py_hash_t * dict_hashes(PyDictKeysObject *keys);
static Py_ssize_t get_index(PyDictKeysObject *dk, Py_ssize_t offset);
static Py_ssize_t keys_nentries(PyDictKeysObject *keys);
static PyObject* dict_iter(PyDictObject *dict);

/*Global counter used to set ma_version_tag field of dictionary.
 * It is incremented each time that a dictionary is created and each
 * time that a dictionary is modified. */
static uint64_t pydict_global_version = 2;

static inline uint64_t
DICT_NEXT_VERSION(void)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->pydict_next_version % 1024 == 0) {
        tstate->pydict_next_version = _Py_atomic_add_uint64(&pydict_global_version, 1024);
    }
    return ++tstate->pydict_next_version;
}

#include "clinic/dictobject.c.h"

void
_PyDict_ClearFreeList(void)
{
}

/* Print summary info about the state of the optimized allocator */
void
_PyDict_DebugMallocStats(FILE *out)
{
}

void
_PyDict_Fini(void)
{
    _PyDict_ClearFreeList();
}

static void free_keys_object(PyDictKeysObject *keys);

static Py_hash_t
compute_hash(PyObject *key)
{
    if (PyUnicode_CheckExact(key)) {
        Py_hash_t hash = ((PyASCIIObject *) key)->hash;
        if (hash != -1) {
            return hash;
        }
    }
    return PyObject_Hash(key);
}

static PyDictKeyEntry empty_entries[7];

/* This immutable, empty PyDictKeysObject is used for PyDict_Clear()
 * (which cannot fail and thus can do no allocation).
 */
static PyDictKeysObject empty_keys_struct = {
        0, /* dk_usable */
        DK_UNICODE, /* dk_type */
        7, /* dk_size */
        empty_entries, /* dk_entries */
        0, /* dk_nentries */
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, /* dk_ctrl */
};

#define Py_EMPTY_KEYS &empty_keys_struct

/* Uncomment to check the dict content in _PyDict_CheckConsistency() */
// #define DEBUG_PYDICT

#ifdef DEBUG_PYDICT
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 1))
#else
#  define ASSERT_CONSISTENT(op) assert(_PyDict_CheckConsistency((PyObject *)(op), 0))
#endif

static Py_ssize_t
usable_fraction(Py_ssize_t size);

static Py_hash_t
dict_entry_hash(PyDictKeysObject *keys, PyDictKeyEntry *entry);

int
_PyDict_CheckConsistency(PyObject *op, int check_content)
{
#define CHECK(expr) \
    do { if (!(expr)) { _PyObject_ASSERT_FAILED_MSG(op, Py_STRINGIFY(expr)); } } while (0)

    assert(op != NULL);
    CHECK(PyDict_Check(op));
    PyDictObject *mp = (PyDictObject *)op;

    // we can only check consistency if dict is locked or brand new
    assert(_PyMutex_is_locked(&mp->ma_mutex) || Py_REFCNT(op) == 1);

    PyDictKeysObject *keys = mp->ma_keys;
    Py_ssize_t usable = usable_fraction(keys->dk_size);

    CHECK(0 <= mp->ma_used && mp->ma_used <= usable);
    CHECK(0 <= keys->dk_usable && keys->dk_usable <= usable);
    CHECK(0 <= keys->dk_nentries && keys->dk_nentries <= usable);
    CHECK(keys->dk_usable + keys->dk_nentries <= usable);

    if (check_content) {
        PyDictKeyEntry *entries = keys->dk_entries;
        if (keys == Py_EMPTY_KEYS) {
            return 1;
        }

        for (Py_ssize_t i = 0, n = keys_nentries(keys); i < n; i++) {
            Py_ssize_t ix = get_index(keys, i);
            CHECK(ix >= 0 && ix < keys->dk_size);
            if (i > 0) {
                assert(ix != get_index(keys, i - 1));
            }
        }

        for (Py_ssize_t i=0; i < keys->dk_size; i++) {
            PyDictKeyEntry *entry = &entries[i];
            uint8_t ctrl = keys->dk_ctrl[i];

            if (ctrl == CTRL_EMPTY || ctrl == CTRL_DELETED) {
                CHECK(entry->me_key == NULL);
                CHECK(entry->me_value == NULL);
            }
            else {
                CHECK((ctrl & CTRL_FULL) == CTRL_FULL);
                PyObject *key = entry->me_key;
                Py_hash_t entry_hash = dict_entry_hash(keys, entry);
                CHECK(entry_hash != -1);
                CHECK(((entry_hash & 0x7F) | CTRL_FULL) == ctrl);
                if (PyUnicode_CheckExact(key)) {
                    Py_hash_t hash = ((PyASCIIObject *)key)->hash;
                    CHECK(entry_hash == hash);
                }
                if (keys->dk_type == DK_GENERIC) {
                    /* test_dict fails if PyObject_Hash() is called again */
                    CHECK(entry_hash == dict_hashes(keys)[i]);
                }
                CHECK(entry->me_value != NULL);
            }
        }
    }
    return 1;

#undef CHECK
}

static uint8_t index_size(Py_ssize_t size)
{
    if (size <= UINT8_MAX) {
        return 1;
    }
    else if (size <= UINT16_MAX) {
        return 2;
    }
    else if (size <= UINT32_MAX) {
        return 4;
    }
    else {
        return sizeof(Py_ssize_t);
    }
}

static int
bsr(Py_ssize_t x)
{
#if defined(_MSC_VER)
  unsigned long idx;
  _BitScanReverse64(&idx, x);
  return (int)idx;
#elif defined(__GNUC__) || defined(__clang__)
    return 63 ^ __builtin_clzl(x);
#else
#endif
}

// The usable_fraction is the maximum dictionary load. It's set to
// 7/8th, rounded up. The ratio is taken from abseil::raw_hash_set.
// Increasing this ratio makes dictionaries more dense resulting in more
// collisions.  Decreasing it improves sparseness at the expense of spreading
// entries over more cache lines and at the cost of total memory consumed.
static Py_ssize_t
usable_fraction(Py_ssize_t size)
{
    if (DICT_GROUP_SIZE == 8 && size == 7) {
        return 6;
    }
    // NOTE: faster with unsigned arithmetic; size is never negative.
    return (size_t)size - (size_t)size / 8;
}

static Py_ssize_t
capacity_from_usable(Py_ssize_t n)
{
    if (n <= PyDict_MINSIZE) {
        return PyDict_MINSIZE;
    }

    int bits = bsr((unsigned long) n);
    return ((size_t)2 << bits) - 1;
}

static void *
dict_indices(PyDictKeysObject *dk)
{
    return (void *)(dk->dk_entries + dk->dk_size);
}

static inline int
key_is_interned(PyObject *key)
{
    return PyUnicode_CheckExact(key) && PyUnicode_CHECK_INTERNED(key);
}

static PyDictKeysObject *
new_keys_object(Py_ssize_t size, uint8_t type)
{
    // TODO: integer overflow! = PyErr_NoMemory
    assert(size >= PyDict_MINSIZE);
    assert(((size + 1) & (size)) == 0 && "size must be one less than a power-of-two");

    Py_ssize_t usable = usable_fraction(size);
    Py_ssize_t ctrl_size = size + 1;
    if (ctrl_size < DICT_GROUP_SIZE) {
        ctrl_size = DICT_GROUP_SIZE;
    }
    if (DICT_GROUP_SIZE == 8 && size == 7) {
        usable = 6;
    }

    size_t hash_size = type == DK_GENERIC ? sizeof(Py_hash_t) * size : 0;

    // mapping from size_shift to number of bytes for each index
    uint8_t ix_size = index_size(size);

    size_t mem_size = sizeof(PyDictKeysObject);
    mem_size += ctrl_size;
    mem_size += hash_size;
    mem_size += sizeof(PyDictKeyEntry) * size;
    mem_size += ix_size * (usable + 1);

    mi_heap_t *heap = _PyThreadState_GET()->heaps[mi_heap_tag_dict_keys];
    PyDictKeysObject *dk = mi_heap_malloc(heap, mem_size);
    if (dk == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    dk->dk_usable = usable;
    _Py_atomic_store_uint8_relaxed(&dk->dk_type, type);
    _Py_atomic_store_ssize_relaxed(&dk->dk_size, size);
    _Py_atomic_store_ssize_relaxed(&dk->dk_nentries, 0);

    // TODO: non-atomic stores
    for (Py_ssize_t i = 0; i != ctrl_size; i++) {
        dk->dk_ctrl[i] = CTRL_EMPTY;
    }
    dk->dk_ctrl[size] = CTRL_DELETED;

    PyDictKeyEntry *entries = (PyDictKeyEntry *)(dk->dk_ctrl + ctrl_size + hash_size);
    _Py_atomic_store_ptr_relaxed(&dk->dk_entries, entries);
    for (Py_ssize_t i = 0; i != size; i++) {
        PyDictKeyEntry *ep = &entries[i];
        _Py_atomic_store_ptr_relaxed(&ep->me_key, NULL);
        _Py_atomic_store_ptr_relaxed(&ep->me_value, NULL);
    }

    return dk;
}

static void
free_keys_object(PyDictKeysObject *keys)
{
    if (keys == Py_EMPTY_KEYS) {
        return;
    }
    PyDictKeyEntry *entries = keys->dk_entries;
    for (Py_ssize_t i = 0, n = keys->dk_size; i < n; i++) {
        if (ctrl_is_full(keys->dk_ctrl[i])) {
            Py_XDECREF(entries[i].me_key);
            Py_XDECREF(entries[i].me_value);
        }
    }
    mi_free(keys);
}

/* Consumes a reference to the keys object */
static PyObject *
new_dict(PyDictKeysObject *keys)
{
    PyDictObject *mp;
    assert(keys != NULL);
    mp = (PyDictObject *)_PyObject_GC_Malloc(sizeof(PyDictObject));
    if (mp == NULL) {
        free_keys_object(keys);
        return NULL;
    }
    PyObject_INIT(mp, &PyDict_Type);
    mp->ma_keys = keys;
    mp->ma_used = 0;
    mp->ma_version_tag = DICT_NEXT_VERSION();
    mp->ma_mutex.v = 0;
    ASSERT_CONSISTENT(mp);
    return (PyObject *)mp;
}

PyObject *
PyDict_New(void)
{
    return new_dict(Py_EMPTY_KEYS);
}

static Py_hash_t
dict_entry_hash(PyDictKeysObject *keys, PyDictKeyEntry *entry)
{
    if (keys->dk_type == DK_UNICODE) {
        PyObject *key = _Py_atomic_load_ptr_relaxed(&entry->me_key);
        return ((PyASCIIObject *) key)->hash;
    }
    else {
        Py_ssize_t idx = entry - keys->dk_entries;
        Py_hash_t *hashes = dict_hashes(keys);
        return _Py_atomic_load_ssize(&hashes[idx]);
    }
}

static Py_hash_t
perturb_hash(PyDictKeysObject *keys, Py_hash_t hash)
{
    if (keys->dk_type == DK_UNICODE) {
        return hash;
    }
    // murmur3 finalizers from https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
    // (public domain)
#if SIZEOF_SIZE_T > 4
    uint64_t k = (uint64_t)hash;
    k ^= k >> 33;
    k *= UINT64_C(0xff51afd7ed558ccd);
    k ^= k >> 33;
    k *= UINT64_C(0xc4ceb9fe1a85ec53);
    k ^= k >> 33;
    return (Py_ssize_t)k;
#else
    uint32_t h = (uint32_t)hash;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return (Py_ssize_t)h;
#endif
}

static PyDictKeyEntry *
find_locked(PyDictObject *mp, PyObject *key, Py_hash_t hash, int *is_error)
{
    assert(_PyMutex_is_locked(&mp->ma_mutex));
    PyDictKeysObject *keys;
    PyDictKeyEntry *entries;
    size_t mask;
    Py_hash_t ix;
    Py_hash_t perturb;
retry:
    keys = mp->ma_keys;
    perturb = perturb_hash(keys, hash);
    entries = keys->dk_entries;
    mask = keys->dk_size & DICT_SIZE_MASK;
    ix = (perturb >> 7) & mask;
    for (;;) {
        dict_ctrl ctrl = load_ctrl(keys, ix);
        dict_bitmask bitmask = dict_match(ctrl, perturb);
        while (bitmask) {
            int lsb = bitmask_lsb(bitmask);
            PyDictKeyEntry *entry = &entries[ix + lsb];
            PyObject *entry_key = entry->me_key;
            if (_PY_LIKELY(entry->me_key == key)) {
                *is_error = 0;
                return entry;
            }
            Py_hash_t entry_hash = dict_entry_hash(keys, entry);
            if (entry_hash != hash) {
                goto next;
            }
            Py_INCREF(entry_key);
            int cmp = PyObject_RichCompareBool(entry_key, key, Py_EQ);
            Py_DECREF(entry_key);
            if (cmp < 0) {
                *is_error = 1;
                return NULL;
            }
            if (mp->ma_keys != keys || entry->me_key != entry_key) {
                goto retry;
            }
            if (cmp == 1) {
                *is_error = 0;
                return entry;
            }
next:
            bitmask &= bitmask - 1;
        }
        if (_PY_LIKELY(ctrl_has_empty(ctrl))) {
            *is_error = 0;
            return NULL;
        }
        ix = (ix + DICT_GROUP_SIZE) & mask;
    }
}

static PyDictKeyEntry *
find(PyDictObject *mp, PyObject *key, Py_hash_t hash)
{
    PyDictKeysObject *keys;
    PyDictKeyEntry *entries;
    size_t mask;
    Py_hash_t ix;
    Py_hash_t perturb;
retry:
    keys = mp->ma_keys;
    perturb = perturb_hash(keys, hash);
    entries = keys->dk_entries;
    mask = keys->dk_size & DICT_SIZE_MASK;
    ix = (perturb >> 7) & mask;
    for (;;) {
        dict_ctrl ctrl = load_ctrl(keys, ix);
        dict_bitmask bitmask = dict_match(ctrl, perturb);
        while (bitmask) {
            int lsb = bitmask_lsb(bitmask);
            PyDictKeyEntry *entry = &entries[ix + lsb];
            PyObject *entry_key = entry->me_key;
            if (_PY_LIKELY(entry_key == key)) {
                return entry;
            }
            Py_hash_t entry_hash = dict_entry_hash(keys, entry);
            if (entry_hash != hash) {
                goto next;
            }
            Py_INCREF(entry_key);
            int cmp = PyObject_RichCompareBool(entry_key, key, Py_EQ);
            Py_DECREF(entry_key);
            if (cmp < 0) {
                return NULL;
            }
            if (mp->ma_keys != keys || entry->me_key != entry_key) {
                goto retry;
            }
            if (cmp == 1) {
                return entry;
            }
next:
            bitmask &= bitmask - 1;
        }
        if (_PY_LIKELY(ctrl_has_empty(ctrl))) {
            return NULL;
        }
        ix = (ix + DICT_GROUP_SIZE) & mask;
    }
}

static PyObject *
pydict_get(PyDictObject *mp, PyObject *key, Py_hash_t hash);

_Py_NO_INLINE static PyObject *
value_for_key_locked(PyDictObject *mp, PyObject *key, Py_hash_t hash)
{
    PyObject *value = NULL;

    if (hash == -1) {
        assert(PyUnicode_CheckExact(key));
        hash = ((PyASCIIObject *)key)->hash;
    }

    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    int is_error;
    PyDictKeyEntry *entry = find_locked(mp, key, hash, &is_error);
    if (entry) {
        value = entry->me_value;
        Py_INCREF(value);
    }
    Py_END_CRITICAL_SECTION;
    return value;
}

_Py_NO_INLINE static PyObject *
value_for_key_retry(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *garbage)
{
    Py_DECREF(garbage);
    return value_for_key_locked(mp, key, hash);
}

static inline PyObject *
value_for_entry(PyDictObject *mp, uint64_t tag, PyObject *key, Py_hash_t hash, PyDictKeyEntry *entry)
{
    PyObject *value = _Py_atomic_load_ptr(&entry->me_value);
    if (_PY_UNLIKELY(value == NULL)) {
        return value_for_key_locked(mp, key, hash);
    }
    if (_PY_LIKELY(_Py_TryIncrefFast(value))) {
        goto check_tag;
    }
    if (_PY_UNLIKELY(!_Py_TryIncRefShared_impl(value))) {
        return value_for_key_locked(mp, key, hash);
    }
    if (_PY_UNLIKELY(value != _Py_atomic_load_ptr(&entry->me_value))) {
        return value_for_key_retry(mp, key, hash, value);
    }
check_tag:
    if (_PY_UNLIKELY(tag != _Py_atomic_load_uint64(&mp->ma_version_tag))) {
        return value_for_key_retry(mp, key, hash, value);
    }
    return value;
}

static PyDictKeyEntry *
prepare_insert(PyDictObject *mp, Py_hash_t hash);

static Py_hash_t *
dict_hashes(PyDictKeysObject *keys) {
    assert(keys->dk_type == DK_GENERIC);
    size_t mask = keys->dk_size & DICT_SIZE_MASK;
    return (Py_hash_t *)(keys->dk_ctrl + mask + DICT_GROUP_SIZE);
}

static void
insert_index(PyDictKeysObject *dk, Py_ssize_t idx)
{
    Py_ssize_t offset = dk->dk_nentries;
    dk->dk_nentries += 1;
    void *indices = dict_indices(dk);
    if (dk->dk_size <= UINT8_MAX) {
        ((uint8_t *)indices)[offset] = idx;
    }
    else if (dk->dk_size <= UINT16_MAX) {
        ((uint16_t *)indices)[offset] = idx;
    }
    else if (dk->dk_size <= UINT32_MAX) {
        ((uint32_t *)indices)[offset] = idx;
    }
    else {
        ((uint64_t *)indices)[offset] = idx;
    }
}

static Py_ssize_t
get_index(PyDictKeysObject *keys, Py_ssize_t offset)
{
    void *indices = dict_indices(keys);
    if (keys->dk_size <= UINT8_MAX) {
        return ((uint8_t *)indices)[offset];
    }
    else if (keys->dk_size <= UINT16_MAX) {
        return ((uint16_t *)indices)[offset];
    }
    else if (keys->dk_size <= UINT32_MAX) {
        return ((uint32_t *)indices)[offset];
    }
    else {
        return ((uint64_t *)indices)[offset];
    }
}

static Py_ssize_t
keys_nentries(PyDictKeysObject *keys)
{
    return keys->dk_nentries;
}

static PyDictKeyEntry *
entry_at(PyDictKeysObject *keys, Py_ssize_t n)
{
    Py_ssize_t idx = get_index(keys, n);
    if (keys->dk_ctrl[idx] == CTRL_DELETED) {
        return NULL;
    }
    return &keys->dk_entries[idx];
}

static PyDictKeyEntry *
next_entry(PyDictKeysObject *keys, Py_ssize_t *i)
{
    Py_ssize_t n = keys_nentries(keys);
    while (*i < n) {
        PyDictKeyEntry *entry = entry_at(keys, *i);
        *i += 1;
        if (entry) {
            return entry;
        }
    }
    return NULL;
}

static PyDictKeyEntry *
prev_entry(PyDictKeysObject *keys, Py_ssize_t *i)
{
    while (*i >= 0) {
        PyDictKeyEntry *entry = entry_at(keys, *i);
        *i -= 1;
        if (entry) {
            return entry;
        }
    }
    return NULL;
}

static PyDictKeyEntry *
find_or_prepare_insert(PyDictObject *mp, PyObject *key, Py_hash_t hash, int *is_insert)
{
    if (mp->ma_keys->dk_type != DK_GENERIC && !key_is_interned(key)) {
        int err = resize(mp, mp->ma_keys->dk_size, DK_GENERIC);
        if (err < 0) {
            return NULL;
        }
    }

    int is_error;
    PyDictKeyEntry *entry = find_locked(mp, key, hash, &is_error);
    if (entry) {
        *is_insert = 0;
        return entry;
    }
    else if (is_error) {
        return NULL;
    }
    *is_insert = 1;
    return prepare_insert(mp, hash);
}

static Py_ssize_t
find_first_non_full(PyDictKeysObject *keys, Py_hash_t perturb)
{
    size_t mask = keys->dk_size & DICT_SIZE_MASK;
    Py_hash_t ix = (perturb >> 7) & mask;
    for (;;) {
        dict_bitmask bitmask = ctrl_match_empty(load_ctrl(keys, ix));
        if (bitmask != 0) {
            int lsb = bitmask_lsb(bitmask);
            return ix + lsb;
        }
        ix = (ix + DICT_GROUP_SIZE) & mask;
    }
}

static PyDictKeyEntry *
insert(PyDictKeysObject *keys, Py_hash_t hash)
{
    Py_hash_t perturb = perturb_hash(keys, hash);
    Py_ssize_t ix = find_first_non_full(keys, perturb);
    _Py_atomic_store_uint8_relaxed(&keys->dk_ctrl[ix], CTRL_FULL | (perturb & 0x7f));
    keys->dk_usable--;
    insert_index(keys, ix);
    if (keys->dk_type == DK_GENERIC) {
        Py_hash_t *hashes = dict_hashes(keys);
        _Py_atomic_store_ssize_relaxed(&hashes[ix], hash);
    }
    return &keys->dk_entries[ix];
}

static PyDictKeyEntry *
prepare_insert(PyDictObject *mp, Py_hash_t hash)
{
    if (_PY_UNLIKELY(mp->ma_keys->dk_usable == 0)) {
        Py_ssize_t new_size = capacity_from_usable(mp->ma_used * 2);
        if (resize(mp, new_size, mp->ma_keys->dk_type) < 0) {
            return NULL;
        }
    }

    _Py_atomic_store_ssize_relaxed(&mp->ma_used, mp->ma_used + 1);
    return insert(mp->ma_keys, hash);
}

_Py_NO_INLINE static PyObject *
pydict_get_slow_blah(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyDictKeysObject *keys, PyDictKeyEntry *entry)
{
    PyObject *entry_key = _Py_atomic_load_ptr(&entry->me_key);
    PyObject *value = _Py_atomic_load_ptr(&entry->me_value);
    if (!entry_key || !value || !_Py_TRY_INCREF(entry_key)) {
        goto retry;
    }
    if (!_Py_TRY_INCREF(value)) {
        Py_DECREF(entry_key);
        goto retry;
    }
    if (_PY_UNLIKELY(entry_key != _Py_atomic_load_ptr(&entry->me_key))) {
        Py_DECREF(entry_key);
        Py_DECREF(value);
        goto retry;
    }
    if (_PY_UNLIKELY(value != _Py_atomic_load_ptr(&entry->me_value))) {
        Py_DECREF(entry_key);
        Py_DECREF(value);
        goto retry;
    }

    int cmp = PyObject_RichCompareBool(entry_key, key, Py_EQ);
    Py_DECREF(entry_key);
    if (cmp < 0) {
        Py_DECREF(value);
        return NULL;
    }
    if (cmp == 1) {
        return value;
    }

retry:
    return value_for_key_locked(mp, key, hash);
}

static PyObject *
pydict_get(PyDictObject *mp, PyObject *key, Py_hash_t hash)
{
    assert(hash != -1);
    uint64_t tag = _Py_atomic_load_uint64(&mp->ma_version_tag);
    PyDictKeysObject *keys = _Py_atomic_load_ptr_relaxed(&mp->ma_keys);
    PyDictKeyEntry *entries = _Py_atomic_load_ptr_relaxed(&keys->dk_entries);
    size_t mask = keys->dk_size & DICT_SIZE_MASK;
    Py_hash_t perturb = perturb_hash(keys, hash);
    Py_hash_t ix = (perturb >> 7) & mask;
    for (;;) {
        dict_ctrl ctrl = load_ctrl(keys, ix);
        dict_bitmask bitmask = dict_match(ctrl, perturb);
        while (bitmask) {
            int lsb = bitmask_lsb(bitmask);
            PyDictKeyEntry *entry = &entries[ix + lsb];
            PyObject *entry_key = _Py_atomic_load_ptr_relaxed(&entry->me_key);
            if (_PY_LIKELY(entry_key == key)) {
                return value_for_entry(mp, tag, key, hash, entry);
            }
            Py_hash_t entry_hash = dict_entry_hash(keys, entry);
            if (entry_hash != hash) {
                goto next;
            }
            return pydict_get_slow_blah(mp, key, hash, keys, entry);
next:
            bitmask &= bitmask - 1;
        }
        if (_PY_LIKELY(ctrl_has_empty(ctrl))) {
            return NULL;
        }
        ix = (ix + DICT_GROUP_SIZE) & mask;
    }
}

int
_PyDict_HasOnlyStringKeys(PyObject *dict)
{
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    assert(PyDict_Check(dict));
    /* Shortcut */
    if (((PyDictObject *)dict)->ma_keys->dk_type != DK_GENERIC)
        return 1;
    while (PyDict_Next(dict, &pos, &key, &value))
        if (!PyUnicode_Check(key))
            return 0;
    return 1;
}

#define MAINTAIN_TRACKING(mp, key, value) \
    do { \
        if (!_PyObject_GC_IS_TRACKED(mp)) { \
            if (_PyObject_GC_MAY_BE_TRACKED(key) || \
                _PyObject_GC_MAY_BE_TRACKED(value)) { \
                _PyObject_GC_TRACK(mp); \
            } \
        } \
    } while(0)

void
_PyDict_MaybeUntrack(PyObject *op)
{
    if (!PyDict_CheckExact(op) || !_PyObject_GC_IS_TRACKED(op)) {
        return;
    }

    PyDictObject *mp = (PyDictObject *) op;
    PyDictKeysObject *keys = mp->ma_keys;
    for (Py_ssize_t i = 0, n = keys->dk_size; i < n; i++) {
        if (!ctrl_is_full(keys->dk_ctrl[i])) {
            continue;
        }
        PyDictKeyEntry *entry = &keys->dk_entries[i];
        if (_PyObject_GC_MAY_BE_TRACKED(entry->me_value) ||
            _PyObject_GC_MAY_BE_TRACKED(entry->me_key)) {
            return;
        }
    }
    _PyObject_GC_UNTRACK(op);
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
If a table is split (its keys and hashes are shared, its values are not),
then the values are temporarily copied into the table, it is resized as
a combined table, then the me_value slots in the old table are NULLed out.
After resizing a table is always combined,
but can be resplit by make_keys_shared().
*/
static int
resize(PyDictObject *mp, Py_ssize_t new_size, uint8_t type) {
    PyDictKeysObject *keys = new_keys_object(new_size, type);
    if (!keys) {
        return -1;
    }

    PyDictKeysObject *oldkeys = mp->ma_keys;
    Py_ssize_t nentries = mp->ma_used;
    for (Py_ssize_t i = 0, j = 0; j < nentries; i++) {
        PyDictKeyEntry *oldentry = entry_at(oldkeys, i);
        if (!oldentry) {
            continue;
        }

        Py_hash_t hash = dict_entry_hash(oldkeys, oldentry);
        PyDictKeyEntry *newentry = insert(keys, hash);
        _Py_atomic_store_ptr_relaxed(&newentry->me_key, oldentry->me_key);
        _Py_atomic_store_ptr_relaxed(&newentry->me_value, oldentry->me_value);
        j++;
    }

    _Py_atomic_store_ptr_release(&mp->ma_keys, keys);
    ASSERT_CONSISTENT(mp);
    if (oldkeys != Py_EMPTY_KEYS) {
        _Py_atomic_store_uint64_release(&mp->ma_version_tag, DICT_NEXT_VERSION());
        _mi_ptr_use_qsbr(oldkeys);
        mi_free(oldkeys);
    }
    return 0;
}

static int
reserve(PyDictObject *mp, Py_ssize_t n)
{
    Py_ssize_t size = capacity_from_usable(n);
    if (size > mp->ma_keys->dk_size) {
        return resize(mp, size, mp->ma_keys->dk_type);
    }
    return 0;
}

static PyObject *
_PyDict_NewPresizedWithType(Py_ssize_t usable, uint8_t type)
{
    Py_ssize_t size = capacity_from_usable(usable);
    PyDictKeysObject *new_keys = new_keys_object(size, type);
    if (new_keys == NULL) {
        return NULL;
    }
    return new_dict(new_keys);
}

PyObject *
_PyDict_NewPresized(Py_ssize_t usable)
{
    if (usable == 0) {
        return PyDict_New();
    }
    return _PyDict_NewPresizedWithType(usable, DK_UNICODE);
}

/* Note that, for historical reasons, PyDict_GetItem() suppresses all errors
 * that may occur (originally dicts supported only string keys, and exceptions
 * weren't possible).  So, while the original intent was that a NULL return
 * meant the key wasn't present, in reality it can mean that, or that an error
 * (suppressed) occurred while computing the key's hash, or that some error
 * (suppressed) occurred when comparing keys in the dict's internal probe
 * sequence.  A nasty example of the latter is when a Python-coded comparison
 * function hits a stack-depth error, which can cause this to return NULL
 * even if the key is present.
 */
PyObject *
PyDict_GetItem(PyObject *op, PyObject *key)
{
    if (!PyDict_Check(op))
        return NULL;

    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        PyErr_Clear();
        return NULL;
    }

    /* We can arrive here with a NULL tstate during initialization: try
       running "python -Wi" for an example related to string interning.
       Let's just hope that no exception occurs then...  This must be
       _PyThreadState_GET() and not PyThreadState_Get() because the latter
       abort Python if tstate is NULL. */
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate != NULL && tstate->curexc_type != NULL) {
        /* preserve the existing exception */
        PyObject *err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        PyObject *value = _PyDict_GetItem_KnownHash(op, key, hash);
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        return value;
    }

    PyObject *value = _PyDict_GetItem_KnownHash(op, key, hash);
    if (!value) {
        PyErr_Clear();
    }
    return value;
}

/* Same as PyDict_GetItemWithError() but with hash supplied by caller.
   This returns NULL *with* an exception set if an exception occurred.
   It returns NULL *without* an exception set if the key wasn't present.
*/
PyObject *
_PyDict_GetItem_KnownHash(PyObject *op, PyObject *key, Py_hash_t hash)
{
    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    assert(hash != -1);
    PyDictKeyEntry *entry = find((PyDictObject *)op, key, hash);
    if (entry != NULL) {
        return entry->me_value;
    }
    return NULL;
}

/* Variant of PyDict_GetItem() that doesn't suppress exceptions.
   This returns NULL *with* an exception set if an exception occurred.
   It returns NULL *without* an exception set if the key wasn't present.
*/
PyObject *
PyDict_GetItemWithError(PyObject *op, PyObject *key)
{
    if (_PY_UNLIKELY(!PyDict_Check(op))) {
        PyErr_BadInternalCall();
        return NULL;
    }
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }
    return _PyDict_GetItem_KnownHash(op, key, hash);
}

_Py_NO_INLINE static PyObject *
PyDict_GetItemWithError2_slow(PyDictObject *mp, PyObject *key)
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }
    return pydict_get(mp, key, hash);
}

_Py_HOT_FUNCTION
PyObject *
PyDict_GetItemWithError2(PyObject *op, PyObject *key)
{
    PyDictObject *mp = (PyDictObject *)op;
    uint64_t tag = _Py_atomic_load_uint64(&mp->ma_version_tag);
    PyDictKeysObject *keys = mp->ma_keys;
    if (_PY_LIKELY(keys->dk_type == DK_UNICODE && key_is_interned(key))) {
        PyDictKeyEntry *entry = find_unicode(keys, key);
        if (entry == NULL) {
            return NULL;
        }
        return value_for_entry(mp, tag, key, -1, entry);
    }
    return PyDict_GetItemWithError2_slow((PyDictObject *)op, key);
}

PyObject *
vm_try_load(PyObject *op, PyObject *key, intptr_t *meta)
{
    if (_PY_UNLIKELY(!PyDict_CheckExact(op))) {
        PyObject *value = PyObject_GetItem(op, key);
        if (_PY_UNLIKELY(value == NULL && PyErr_Occurred())) {
            if (PyErr_ExceptionMatches(PyExc_KeyError)) {
                PyErr_Clear();
            }
        }
        return value;
    }
    PyDictObject *mp = (PyDictObject *)op;
    uint64_t tag = _Py_atomic_load_uint64(&mp->ma_version_tag);
    PyDictKeysObject *keys = _Py_atomic_load_ptr(&mp->ma_keys);
    if (_PY_UNLIKELY(keys->dk_type != DK_UNICODE)) {
        return PyDict_GetItemWithError2(op, key);
    }
    assert(keys->dk_type == DK_UNICODE); // for now
    PyDictKeyEntry *entry = find_unicode(keys, key);
    if (entry == NULL) {
        if (tag <= INTPTR_MAX) {
            // A negative value (other than -1) indicates the key is not
            // present in the dict with the given version_tag.
            _Py_atomic_store_intptr_relaxed(meta,  -((intptr_t)tag));
        }
        return NULL;
    }
    intptr_t offset = (intptr_t)(entry - keys->dk_entries);
    _Py_atomic_store_intptr_relaxed(meta, offset);
    return value_for_entry(mp, tag, key, -1, entry);
}

PyObject *
vm_load_global(PyThreadState *ts, PyObject *key, intptr_t *meta)
{
    assert(PyUnicode_CheckExact(key) && PyUnicode_CHECK_INTERNED(key));
    _Py_atomic_store_intptr_relaxed(meta, -1);
    PyFunctionObject *func = (PyFunctionObject *)AS_OBJ(ts->regs[-1]);
    PyObject *res = vm_try_load(func->globals, key, meta);
    if (res != NULL || PyErr_Occurred()) {
        return res;
    }
    res = vm_try_load(func->builtins, key, meta + 1);
    if (res != NULL || PyErr_Occurred()) {
        return res;
    }
    return vm_err_name(ts, 0);
}

PyObject *
_PyDict_GetItemIdWithError(PyObject *dp, struct _Py_Identifier *key)
{
    PyObject *kv;
    kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return NULL;
    Py_hash_t hash = ((PyASCIIObject *) kv)->hash;
    assert (hash != -1);  /* interned strings have their hash value initialised */
    return _PyDict_GetItem_KnownHash(dp, kv, hash);
}

PyObject *
_PyDict_GetItemStringWithError(PyObject *v, const char *key)
{
    PyObject *kv, *rv;
    kv = PyUnicode_FromString(key);
    if (kv == NULL) {
        return NULL;
    }
    rv = PyDict_GetItemWithError(v, kv);
    Py_DECREF(kv);
    return rv;
}

static int
assign(PyDictObject *mp, PyObject *key, Py_hash_t hash, PyObject *value)
{
    int ret = 0;
    int is_insert;
    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    PyDictKeyEntry *entry = find_or_prepare_insert(mp, key, hash, &is_insert);
    if (!entry) {
        ret = -1;
        goto exit;
    }
    MAINTAIN_TRACKING(mp, key, value);
    if (is_insert) {
        Py_INCREF(key);
        Py_INCREF(value);
        _Py_atomic_store_ptr_relaxed(&entry->me_key, key);
        _Py_atomic_store_ptr_relaxed(&entry->me_value, value);
        _Py_atomic_store_uint64_release(&mp->ma_version_tag, DICT_NEXT_VERSION());
    }
    else {
        PyObject *old = entry->me_value;
        if (old == value) {
            goto exit;
        }
        Py_INCREF(value);
        _Py_atomic_store_ptr_relaxed(&entry->me_value, value);
        _Py_atomic_store_uint64_release(&mp->ma_version_tag, DICT_NEXT_VERSION());
        Py_DECREF(old);
    }
    ASSERT_CONSISTENT(mp);
exit:
    Py_END_CRITICAL_SECTION;
    return ret;
}

static void
finish_erase(PyDictObject *mp, PyDictKeyEntry *entry)
{
    PyDictKeysObject *keys = mp->ma_keys;
    Py_ssize_t idx = (Py_ssize_t)(entry - keys->dk_entries);
    _Py_atomic_store_uint8_relaxed(&keys->dk_ctrl[idx], CTRL_DELETED);
    _Py_atomic_store_ssize_relaxed(&mp->ma_used, mp->ma_used - 1);
    PyObject *oldkey = entry->me_key;
    PyObject *oldvalue = entry->me_value;
    _Py_atomic_store_ptr_relaxed(&entry->me_key, NULL);
    _Py_atomic_store_ptr_relaxed(&entry->me_value, NULL);
    _Py_atomic_store_uint64_relaxed(&mp->ma_version_tag, DICT_NEXT_VERSION());
    Py_DECREF(oldkey);
    Py_DECREF(oldvalue);
}

static int
erase(PyDictObject *mp, PyObject *key, Py_hash_t hash)
{
    int ret, is_error;

    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    PyDictKeyEntry *entry = find_locked(mp, key, hash, &is_error);
    if (entry != NULL) {
        ret = 0;
        finish_erase(mp, entry);
    }
    else {
        ret = -1;
        if (!is_error) {
            _PyErr_SetKeyError(key);
        }
    }
    Py_END_CRITICAL_SECTION;
    return ret;
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
PyDict_SetItem(PyObject *op, PyObject *key, PyObject *value)
{
    if (_PY_UNLIKELY(!PyDict_Check(op))) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    assert(value);
    Py_ssize_t hash = compute_hash(key);
    if (hash == -1) {
        return -1;
    }
    return assign((PyDictObject *)op, key, hash, value);
}

int
_PyDict_SetItem_KnownHash(PyObject *op, PyObject *key, PyObject *value,
                         Py_hash_t hash)
{
    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    assert(value);
    assert(hash != -1);
    return assign((PyDictObject *)op, key, hash, value);
}

int
PyDict_DelItem(PyObject *op, PyObject *key)
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return -1;
    }
    return _PyDict_DelItem_KnownHash(op, key, hash);
}

int
_PyDict_DelItem_KnownHash(PyObject *op, PyObject *key, Py_hash_t hash)
{
    return erase((PyDictObject *)op, key, hash);
}

/* This function promises that the predicate -> deletion sequence is atomic
 * (i.e. protected by the dictionary mutex), assuming the predicate itself doesn't
 * release the GIL.
 */
int
_PyDict_DelItemIf(PyObject *op, PyObject *key,
                  int (*predicate)(PyObject *value, void *data),
                  void *data)
{
    int ret = 0, is_error;

    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }

    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return -1;
    }

    PyDictObject *mp = (PyDictObject *)op;
    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    PyDictKeyEntry *ep = find_locked(mp, key, hash, &is_error);
    if (!ep) {
        if (!is_error) {
            _PyErr_SetKeyError(key);
        }
        ret = -1;
        goto exit;
    }

    PyObject *old_value = ep->me_value;
    if (predicate(old_value, data)) {
        finish_erase(mp, ep);
    }

exit:
    Py_END_CRITICAL_SECTION;
    return ret;
}


void
PyDict_Clear(PyObject *op)
{
    if (!PyDict_Check(op)) {
        return;
    }
    PyDictObject *mp = ((PyDictObject *)op);
    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    PyDictKeysObject *oldkeys = mp->ma_keys;
    if (oldkeys != Py_EMPTY_KEYS) {
        /* Empty the dict... */
        _Py_atomic_store_ptr_relaxed(&mp->ma_keys, Py_EMPTY_KEYS);
        _Py_atomic_store_ssize_relaxed(&mp->ma_used, 0);
        _Py_atomic_store_uint64_relaxed(&mp->ma_version_tag, DICT_NEXT_VERSION());
        ASSERT_CONSISTENT(mp);

        /* ...then clear the keys and values */
        _mi_ptr_use_qsbr(oldkeys);
        free_keys_object(oldkeys);
    }
    Py_END_CRITICAL_SECTION;
}

/* Internal version of PyDict_Next that returns a hash value in addition
 * to the key and value.
 * Return 1 on success, return 0 when the reached the end of the dictionary
 * (or if op is not a dictionary)
 */
int
_PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey,
             PyObject **pvalue, Py_hash_t *phash)
{
    if (!PyDict_Check(op)) {
        return 0;
    }

    Py_ssize_t i = *ppos;
    PyDictObject *mp = (PyDictObject *)op;
    PyDictKeysObject *keys = mp->ma_keys;
    Py_ssize_t n = keys_nentries(keys);
    if (i < 0 || i >= n) {
        return 0;
    }

    // advances ppos
    PyDictKeyEntry *entry = next_entry(keys, ppos);
    if (!entry) {
        assert(*ppos == n);
        return 0;
    }

    PyObject *key = entry->me_key;
    PyObject *value = entry->me_value;
    if (pkey) {
        *pkey = key;
    }
    if (pvalue) {
        *pvalue = value;
    }
    if (phash) {
        *phash = dict_entry_hash(keys, entry);
    }
    return 1;
}

/*
 * Iterate over a dict.  Use like so:
 *
 *     Py_ssize_t i;
 *     PyObject *key, *value;
 *     i = 0;   # important!  i should not otherwise be changed by you
 *     while (PyDict_Next(yourdict, &i, &key, &value)) {
 *         Refer to borrowed references in key and value.
 *     }
 *
 * Return 1 on success, return 0 when the reached the end of the dictionary
 * (or if op is not a dictionary)
 *
 * CAUTION:  In general, it isn't safe to use PyDict_Next in a loop that
 * mutates the dict.  One exception:  it is safe if the loop merely changes
 * the values associated with the keys (but doesn't insert new keys or
 * delete keys), via PyDict_SetItem().
 */
int
PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
    return _PyDict_Next(op, ppos, pkey, pvalue, NULL);
}

/* Internal version of dict.pop(). */
PyObject *
_PyDict_Pop_KnownHash(PyObject *dict, PyObject *key, Py_hash_t hash, PyObject *deflt)
{
    PyDictObject *mp = (PyDictObject *)dict;
    PyObject *value = NULL;

    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    int is_error;
    PyDictKeyEntry *entry = find_locked(mp, key, hash, &is_error);
    if (entry) {
        value = entry->me_value;
        Py_INCREF(value);
        finish_erase(mp, entry);
    }
    else if (!is_error) {
        if (deflt) {
            Py_INCREF(deflt);
            value = deflt;
        }
        else {
            _PyErr_SetKeyError(key);
        }
    }
    Py_END_CRITICAL_SECTION;
    return value;
}

PyObject *
_PyDict_Pop(PyObject *dict, PyObject *key, PyObject *deflt)
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }
    return _PyDict_Pop_KnownHash(dict, key, hash, deflt);
}

/* Internal version of dict.from_keys().  It is subclass-friendly. */
PyObject *
_PyDict_FromKeys(PyObject *cls, PyObject *iterable, PyObject *value)
{
    if (cls == (PyObject *)&PyDict_Type) {
        if (PyDict_CheckExact(iterable)) {
            PyDictObject *src = (PyDictObject *)iterable;
            PyObject *d = _PyDict_NewPresizedWithType(PyDict_GET_SIZE(iterable), src->ma_keys->dk_type);
            if (d == NULL) {
                return NULL;
            }
            PyDictObject *mp = (PyDictObject *)d;

            Py_ssize_t pos = 0;
            PyDictKeyEntry *entry;
            while ((entry = next_entry(src->ma_keys, &pos)) != NULL) {
                Py_hash_t hash = dict_entry_hash(src->ma_keys, entry);
                PyDictKeyEntry *dst = prepare_insert(mp, hash);
                if (!dst) {
                    Py_DECREF(d);
                    return NULL;
                }

                Py_INCREF(entry->me_key);
                dst->me_key = entry->me_key;
                Py_INCREF(value);
                dst->me_value = value;
            }

            return d;
        }
        if (PyAnySet_CheckExact(iterable)) {
            PyObject *d = _PyDict_NewPresized(PySet_GET_SIZE(iterable));
            if (d == NULL) {
                return NULL;
            }
            PyDictObject *mp = (PyDictObject *)d;
            Py_ssize_t pos = 0;
            PyObject *key;
            Py_hash_t hash;

            // FIXME: _PySet_NextEntry should incref key
            while (_PySet_NextEntry(iterable, &pos, &key, &hash)) {
                Py_INCREF(key);
                if (assign(mp, key, hash, value)) {
                    Py_DECREF(key);
                    Py_DECREF(d);
                    return NULL;
                }
                Py_DECREF(key);
            }
            return d;
        }
    }

    PyObject *d = _PyObject_CallNoArg(cls);
    if (d == NULL)
        return NULL;

    PyObject *it = PyObject_GetIter(iterable);
    if (it == NULL){
        Py_DECREF(d);
        return NULL;
    }

    if (PyDict_CheckExact(d)) {
        PyObject *key;
        while ((key = PyIter_Next(it)) != NULL) {
            int status = PyDict_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    } else {
        PyObject *key;
        while ((key = PyIter_Next(it)) != NULL) {
            int status = PyObject_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    }

    if (PyErr_Occurred())
        goto Fail;
    Py_DECREF(it);
    return d;

Fail:
    Py_DECREF(it);
    Py_DECREF(d);
    return NULL;
}

/* Methods */

static void
dict_dealloc(PyDictObject *mp)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyObject_GC_UnTrack(mp);

    Py_TRASHCAN_BEGIN(mp, dict_dealloc)
    free_keys_object(mp->ma_keys);
    Py_TYPE(mp)->tp_free((PyObject *)mp);
    Py_TRASHCAN_END
}


static PyObject *
dict_repr(PyDictObject *mp)
{
    Py_ssize_t i;
    PyObject *key = NULL, *value = NULL;
    _PyUnicodeWriter writer;
    int first;

    i = Py_ReprEnter((PyObject *)mp);
    if (i != 0) {
        return i > 0 ? PyUnicode_FromString("{...}") : NULL;
    }

    if (mp->ma_used == 0) {
        Py_ReprLeave((PyObject *)mp);
        return PyUnicode_FromString("{}");
    }

    _PyUnicodeWriter_Init(&writer);
    writer.overallocate = 1;
    /* "{" + "1: 2" + ", 3: 4" * (len - 1) + "}" */
    writer.min_length = 1 + 4 + (2 + 4) * (mp->ma_used - 1) + 1;

    if (_PyUnicodeWriter_WriteChar(&writer, '{') < 0)
        goto error;

    /* Do repr() on each key+value pair, and insert ": " between them.
       Note that repr may mutate the dict. */
    i = 0;
    first = 1;
    while (PyDict_Next((PyObject *)mp, &i, &key, &value)) {
        PyObject *s;
        int res;

        /* Prevent repr from deleting key or value during key format. */
        Py_INCREF(key);
        Py_INCREF(value);

        if (!first) {
            if (_PyUnicodeWriter_WriteASCIIString(&writer, ", ", 2) < 0)
                goto error;
        }
        first = 0;

        s = PyObject_Repr(key);
        if (s == NULL)
            goto error;
        res = _PyUnicodeWriter_WriteStr(&writer, s);
        Py_DECREF(s);
        if (res < 0)
            goto error;

        if (_PyUnicodeWriter_WriteASCIIString(&writer, ": ", 2) < 0)
            goto error;

        s = PyObject_Repr(value);
        if (s == NULL)
            goto error;
        res = _PyUnicodeWriter_WriteStr(&writer, s);
        Py_DECREF(s);
        if (res < 0)
            goto error;

        Py_CLEAR(key);
        Py_CLEAR(value);
    }

    writer.overallocate = 0;
    if (_PyUnicodeWriter_WriteChar(&writer, '}') < 0)
        goto error;

    Py_ReprLeave((PyObject *)mp);

    return _PyUnicodeWriter_Finish(&writer);

error:
    Py_ReprLeave((PyObject *)mp);
    _PyUnicodeWriter_Dealloc(&writer);
    Py_XDECREF(key);
    Py_XDECREF(value);
    return NULL;
}

static Py_ssize_t
dict_length(PyDictObject *mp)
{
    return _Py_atomic_load_ssize_relaxed(&mp->ma_used);
}

_Py_NO_INLINE static PyObject *
dict_lookup_missing(PyDictObject *mp, PyObject *key)
{
    if (!PyDict_CheckExact(mp)) {
        /* Look up __missing__ method if we're a subclass. */
        PyObject *missing, *res;
        _Py_IDENTIFIER(__missing__);
        missing = _PyObject_LookupSpecial((PyObject *)mp, &PyId___missing__);
        if (missing != NULL) {
            res = PyObject_CallOneArg(missing, key);
            Py_DECREF(missing);
            return res;
        }
        else if (PyErr_Occurred()) {
            return NULL;
        }
    }
    _PyErr_SetKeyError(key);
    return NULL;
}

 _Py_NO_INLINE static PyObject *
dict_subscript_slow(PyDictObject *mp, PyObject *key)
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }
    PyObject *item = pydict_get(mp, key, hash);
    if (item) {
        return item;
    }
    else if (!item && PyErr_Occurred()) {
        return NULL;
    }
    return dict_lookup_missing(mp, key);
}

static PyObject *
dict_subscript(PyDictObject *mp, PyObject *key)
{
    uint64_t tag = _Py_atomic_load_uint64(&mp->ma_version_tag);
    PyDictKeysObject *keys = mp->ma_keys;
    if (_PY_LIKELY(keys->dk_type == DK_UNICODE && key_is_interned(key))) {
        PyDictKeyEntry *entry = find_unicode(keys, key);
        if (_PY_LIKELY(entry != NULL)) {
            PyObject *value = value_for_entry(mp, tag, key, -1, entry);
            if (_PY_LIKELY(value != NULL)) {
                return value;
            }
        }
        return dict_lookup_missing(mp, key);
    }
    return dict_subscript_slow(mp, key);
}

static int
dict_ass_sub(PyDictObject *mp, PyObject *v, PyObject *w)
{
    if (w == NULL) {
        return PyDict_DelItem((PyObject *)mp, v);
    }
    else {
        return PyDict_SetItem((PyObject *)mp, v, w);
    }
}

static PyMappingMethods dict_as_mapping = {
    (lenfunc)dict_length, /*mp_length*/
    (binaryfunc)dict_subscript, /*mp_subscript*/
    (objobjargproc)dict_ass_sub, /*mp_ass_subscript*/
};

static inline PyObject *
read_entry(PyDictObject *mp, PyDictKeysObject *keys, PyObject **ptr)
{
    PyObject *value = _Py_atomic_load_ptr(ptr);
    if (value == NULL || !_Py_TRY_INCREF(value)) {
        return NULL;
    }
    if (value != _Py_atomic_load_ptr(ptr) || keys != _Py_atomic_load_ptr(&mp->ma_keys)) {
        Py_DECREF(value);
        return NULL;
    }
    return value;
}

static PyObject *
dict_keys(PyDictObject *mp)
{
    PyObject *v;
    Py_ssize_t n;

  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }

    PyDictKeysObject *keys = mp->ma_keys;
    for (Py_ssize_t i = 0, j = 0; j < n; j++) {
        PyDictKeyEntry *entry = next_entry(keys, &i);
        if (entry == NULL) {
            goto fail;
        }
        PyObject *key = read_entry(mp, keys, &entry->me_key);
        if (key == NULL) {
            goto fail;
        }
        PyList_SET_ITEM(v, j, key);
    }
    return v;

fail:
    Py_DECREF(v);
    PyErr_SetString(PyExc_RuntimeError,
                    "dict mutated during iteration");
    return NULL;
}

static PyObject *
dict_values(PyDictObject *mp)
{
    PyObject *v;
    Py_ssize_t n;

  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL) {
        return NULL;
    }
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }

    PyDictKeysObject *keys = mp->ma_keys;
    for (Py_ssize_t i = 0, j = 0; j < n; j++) {
        PyDictKeyEntry *entry = next_entry(keys, &i);
        if (entry == NULL) {
            goto fail;
        }
        PyObject *value = read_entry(mp, keys, &entry->me_value);
        if (value == NULL) {
            goto fail;
        }
        PyList_SET_ITEM(v, j, value);
    }
    return v;

fail:
    Py_DECREF(v);
    PyErr_SetString(PyExc_RuntimeError,
                    "dict mutated during iteration");
    return NULL;
}

static PyObject *
dict_items(PyDictObject *mp)
{
    PyObject *v;
    Py_ssize_t n;

    /* Preallocate the list of tuples, to avoid allocations during
     * the loop over the items, which could trigger GC, which
     * could resize the dict. :-(
     */
  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyTuple_New(2);
        if (item == NULL) {
            Py_DECREF(v);
            return NULL;
        }
        PyList_SET_ITEM(v, i, item);
    }
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }
    /* Nothing we do below makes any function calls. */
    PyDictKeysObject *keys = mp->ma_keys;
    for (Py_ssize_t i = 0, j = 0; j < n; j++) {
        PyDictKeyEntry *entry = next_entry(keys, &i);
        if (entry == NULL) {
            goto fail;
        }
        PyObject *item = PyList_GET_ITEM(v, j);
        PyObject *key = read_entry(mp, keys, &entry->me_key);
        PyObject *value = read_entry(mp, keys, &entry->me_value);
        PyTuple_SET_ITEM(item, 0, key);
        PyTuple_SET_ITEM(item, 1, value);
        if (key == NULL || value == NULL) {
            goto fail;
        }
    }
    return v;

fail:
    Py_DECREF(v);
    PyErr_SetString(PyExc_RuntimeError,
                    "dict mutated during iteration");
    return NULL;
}

/*[clinic input]
@classmethod
dict.fromkeys
    iterable: object
    value: object=None
    /

Create a new dictionary with keys from iterable and values set to value.
[clinic start generated code]*/

static PyObject *
dict_fromkeys_impl(PyTypeObject *type, PyObject *iterable, PyObject *value)
/*[clinic end generated code: output=8fb98e4b10384999 input=382ba4855d0f74c3]*/
{
    return _PyDict_FromKeys((PyObject *)type, iterable, value);
}

/* Single-arg dict update; used by dict_update_common and operators. */
static int
dict_update_arg(PyObject *self, PyObject *arg)
{
    if (PyDict_CheckExact(arg)) {
        return PyDict_Merge(self, arg, 1);
    }
    _Py_IDENTIFIER(keys);
    PyObject *func;
    if (_PyObject_LookupAttrId(arg, &PyId_keys, &func) < 0) {
        return -1;
    }
    if (func != NULL) {
        Py_DECREF(func);
        return PyDict_Merge(self, arg, 1);
    }
    return PyDict_MergeFromSeq2(self, arg, 1);
}

static int
dict_update_common(PyObject *self, PyObject *args, PyObject *kwds,
                   const char *methname)
{
    PyObject *arg = NULL;
    int result = 0;

    if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg)) {
        result = -1;
    }
    else if (arg != NULL) {
        result = dict_update_arg(self, arg);
    }

    if (result == 0 && kwds != NULL) {
        if (PyArg_ValidateKeywordArguments(kwds))
            result = PyDict_Merge(self, kwds, 1);
        else
            result = -1;
    }
    return result;
}

/* Note: dict.update() uses the METH_VARARGS|METH_KEYWORDS calling convention.
   Using METH_FASTCALL|METH_KEYWORDS would make dict.update(**dict2) calls
   slower, see the issue #29312. */
static PyObject *
dict_update(PyObject *self, PyObject *args, PyObject *kwds)
{
    if (dict_update_common(self, args, kwds, "update") != -1)
        Py_RETURN_NONE;
    return NULL;
}

/* Update unconditionally replaces existing items.
   Merge has a 3rd argument 'override'; if set, it acts like Update,
   otherwise it leaves existing items unchanged.

   PyDict_{Update,Merge} update/merge from a mapping object.

   PyDict_MergeFromSeq2 updates/merges from any iterable object
   producing iterable objects of length 2.
*/

int
PyDict_MergeFromSeq2(PyObject *d, PyObject *seq2, int override)
{
    PyObject *it;       /* iter(seq2) */
    Py_ssize_t i;       /* index into seq2 of current element */
    PyObject *item;     /* seq2[i] */
    PyObject *fast;     /* item as a 2-tuple or 2-list */

    assert(d != NULL);
    assert(PyDict_Check(d));
    assert(seq2 != NULL);

    it = PyObject_GetIter(seq2);
    if (it == NULL)
        return -1;

    for (i = 0; ; ++i) {
        PyObject *key, *value;
        Py_ssize_t n;

        fast = NULL;
        item = PyIter_Next(it);
        if (item == NULL) {
            if (PyErr_Occurred())
                goto Fail;
            break;
        }

        /* Convert item to sequence, and verify length 2. */
        fast = PySequence_Fast(item, "");
        if (fast == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError))
                PyErr_Format(PyExc_TypeError,
                    "cannot convert dictionary update "
                    "sequence element #%zd to a sequence",
                    i);
            goto Fail;
        }
        n = PySequence_Fast_GET_SIZE(fast);
        if (n != 2) {
            PyErr_Format(PyExc_ValueError,
                         "dictionary update sequence element #%zd "
                         "has length %zd; 2 is required",
                         i, n);
            goto Fail;
        }

        /* Update/merge with this (key, value) pair. */
        key = PySequence_Fast_GET_ITEM(fast, 0);
        value = PySequence_Fast_GET_ITEM(fast, 1);
        Py_INCREF(key);
        Py_INCREF(value);
        if (override) {
            if (PyDict_SetItem(d, key, value) < 0) {
                Py_DECREF(key);
                Py_DECREF(value);
                goto Fail;
            }
        }
        else if (PyDict_GetItemWithError(d, key) == NULL) {
            if (PyErr_Occurred() || PyDict_SetItem(d, key, value) < 0) {
                Py_DECREF(key);
                Py_DECREF(value);
                goto Fail;
            }
        }

        Py_DECREF(key);
        Py_DECREF(value);
        Py_DECREF(fast);
        Py_DECREF(item);
    }

    i = 0;
    goto Return;
Fail:
    Py_XDECREF(item);
    Py_XDECREF(fast);
    i = -1;
Return:
    Py_DECREF(it);
    return Py_SAFE_DOWNCAST(i, Py_ssize_t, int);
}

// Fast merge if b is also a PyDictObject and uses normal iteration
static int
dict_merge_dict(PyDictObject *a, PyDictObject *b, int override)
{
    int ret = 0;

    Py_ssize_t lenb = dict_length(b);
    if (b == a || lenb == 0) {
        /* a.update(a) or a.update({}); nothing to do */
        return 0;
    }

    Py_BEGIN_CRITICAL_SECTION(&a->ma_mutex);
    if (dict_length(a) == 0) {
        /* Since the target dict is empty, PyDict_GetItem()
         * always returns NULL.  Setting override to 1
         * skips the unnecessary test.
         */
        override = 1;
    }
    /* Do one big resize at the start, rather than
     * incrementally resizing as we insert new items.  Expect
     * that there will be no (or few) overlapping keys.
     */
    if (usable_fraction(a->ma_keys->dk_size) < lenb) {
        if (reserve(a, a->ma_used + lenb)) {
            ret = -1;
            goto exit;
        }
    }

    Py_ssize_t i = 0;
    PyDictKeysObject *keysb = _Py_atomic_load_ptr(&b->ma_keys);
    uint64_t version_tag = b->ma_version_tag;
    PyDictKeyEntry *entry;
    while ((entry = next_entry(keysb, &i))) {
        PyObject *key = read_entry(b, keysb, &entry->me_key);
        PyObject *value = read_entry(b, keysb, &entry->me_value);
        Py_hash_t hash = dict_entry_hash(keysb, entry);
        if (key == NULL || value == NULL) {
            Py_XDECREF(key);
            Py_XDECREF(value);
            PyErr_SetString(PyExc_RuntimeError,
                            "dict mutated during update");
            ret = -1;
            goto exit;
        }

        int is_insert;
        PyDictKeyEntry *dst = find_or_prepare_insert(a, key, hash, &is_insert);
        if (dst == NULL) {
            Py_DECREF(key);
            Py_DECREF(value);
            ret = -1;
            goto exit;
        }
        if (override == 2 && !is_insert) {
            Py_DECREF(key);
            Py_DECREF(value);
            _PyErr_SetKeyError(key);
            ret = -1;
            goto exit;
        }

        MAINTAIN_TRACKING(a, key, value);
        if (is_insert) {
            _Py_atomic_store_ptr_relaxed(&dst->me_key, key);
            _Py_atomic_store_ptr_relaxed(&dst->me_value, value);
            _Py_atomic_store_uint64_relaxed(&a->ma_version_tag, DICT_NEXT_VERSION());
        }
        else if (override == 1) {
            PyObject *tmpval = dst->me_value;
            if (tmpval != value) {
                _Py_atomic_store_ptr_relaxed(&dst->me_value, value);
                _Py_atomic_store_uint64_relaxed(&a->ma_version_tag, DICT_NEXT_VERSION());
            }
            Py_DECREF(key);
            Py_DECREF(tmpval);
        }
        else {
            assert(override == 0);
            Py_DECREF(key);
            Py_DECREF(value);
        }

        if (version_tag != b->ma_version_tag ||
            keysb != _Py_atomic_load_ptr(&b->ma_keys)) {
            PyErr_SetString(PyExc_RuntimeError,
                            "dict mutated during update");
            ret = -1;
            goto exit;
        }
    }
    ASSERT_CONSISTENT(a);

exit:
    Py_END_CRITICAL_SECTION;
    return ret;
}

static int
dict_merge(PyObject *a, PyObject *b, int override)
{
    // override = 0 don't replace entries for existing keys in `a`
    // override = 1 replace existing entries for existing keys in `a`
    // override = 2 error when encountering existing keys in `a`
    assert(0 <= override && override <= 2);

    /* We accept for the argument either a concrete dictionary object,
     * or an abstract "mapping" object.  For the former, we can do
     * things quite efficiently.  For the latter, we only require that
     * PyMapping_Keys() and PyObject_GetItem() be supported.
     */
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    if (PyDict_Check(b) && (Py_TYPE(b)->tp_iter == (getiterfunc)dict_iter)) {
        return dict_merge_dict((PyDictObject*)a, (PyDictObject *)b, override);
    }

    /* Do it the generic, slower way */
    PyObject *keys = PyMapping_Keys(b);
    PyObject *iter;
    PyObject *key, *value;
    int status;

    if (keys == NULL)
        /* Docstring says this is equivalent to E.keys() so
         * if E doesn't have a .keys() method we want
         * AttributeError to percolate up.  Might as well
         * do the same for any other error.
         */
        return -1;

    iter = PyObject_GetIter(keys);
    Py_DECREF(keys);
    if (iter == NULL)
        return -1;

    for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
        if (override != 1) {
            int cmp = PyDict_Contains(a, key);
            if (cmp < 0) {
                Py_DECREF(key);
                Py_DECREF(iter);
                return -1;
            }
            if (cmp == 1) {
                if (override == 0) {
                    Py_DECREF(key);
                    continue;
                }
                assert(override == 2);
                _PyErr_SetKeyError(key);
                Py_DECREF(key);
                Py_DECREF(iter);
                return -1;
            }
        }
        value = PyObject_GetItem(b, key);
        if (value == NULL) {
            Py_DECREF(iter);
            Py_DECREF(key);
            return -1;
        }
        status = PyDict_SetItem(a, key, value);
        Py_DECREF(key);
        Py_DECREF(value);
        if (status < 0) {
            Py_DECREF(iter);
            return -1;
        }
    }
    Py_DECREF(iter);
    if (PyErr_Occurred())
        /* Iterator completed, via error */
        return -1;
    return 0;
}

int
PyDict_Update(PyObject *a, PyObject *b)
{
    return dict_merge(a, b, 1);
}

int
PyDict_Merge(PyObject *a, PyObject *b, int override)
{
    /* XXX Deprecate override not in (0, 1). */
    return dict_merge(a, b, override != 0);
}

int
_PyDict_MergeEx(PyObject *a, PyObject *b, int override)
{
    return dict_merge(a, b, override);
}

static PyObject *
dict_copy(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    return PyDict_Copy((PyObject*)mp);
}

PyObject *
PyDict_Copy(PyObject *o)
{
    PyObject *copy;
    PyDictObject *mp;

    if (o == NULL || !PyDict_Check(o)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    mp = (PyDictObject *)o;
    if (mp->ma_used == 0) {
        /* The dict is empty; just return a new dict. */
        return PyDict_New();
    }

    /* Use fast-copy if:

        (1) 'mp' is an instance of a subclassed dict; and

        (2) 'mp' is not a split-dict; and

        (3) if 'mp' is non-compact ('del' operation does not resize dicts),
            do fast-copy only if it has at most 1/3 non-used keys.

        The last condition (3) is important to guard against a pathological
        case when a large dict is almost emptied with multiple del/pop
        operations and copied after that.  In cases like this, we defer to
        PyDict_Merge, which produces a compacted copy.
    */
    // return clone_combined_dict(mp);

    copy = PyDict_New();
    if (copy == NULL)
        return NULL;
    if (PyDict_Merge(copy, o, 1) == 0)
        return copy;
    Py_DECREF(copy);
    return NULL;
}

Py_ssize_t
PyDict_Size(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return ((PyDictObject *)mp)->ma_used;
}

PyObject *
PyDict_Keys(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_keys((PyDictObject *)mp);
}

PyObject *
PyDict_Values(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_values((PyDictObject *)mp);
}

PyObject *
PyDict_Items(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_items((PyDictObject *)mp);
}

/* Return 1 if dicts equal, 0 if not, -1 if error.
 * Gets out as soon as any difference is detected.
 * Uses only Py_EQ comparison.
 */
static int
dict_equal(PyDictObject *a, PyDictObject *b)
{
    Py_ssize_t i;

    if (a->ma_used != b->ma_used)
        /* can't be equal if # of entries differ */
        return 0;

    /* Same # of entries -- check all of 'em.  Exit early on any diff. */
    PyDictKeysObject *a_keys = _Py_atomic_load_ptr(&a->ma_keys);
    for (i = 0; i < a_keys->dk_size; i++) {
        if (!ctrl_is_full(a_keys->dk_ctrl[i])) {
            continue;
        }

        PyDictKeyEntry *a_entry = &a_keys->dk_entries[i];
        Py_hash_t hash = dict_entry_hash(a_keys, a_entry);
        PyObject *key = read_entry(a, a_keys, &a_entry->me_key);
        PyObject *a_val = read_entry(a, a_keys, &a_entry->me_value);
        if (key == NULL || a_val == NULL) {
            Py_XDECREF(key);
            Py_XDECREF(a_val);
            return 0;
        }

        PyDictKeysObject *b_keys = _Py_atomic_load_ptr(&b->ma_keys);
        PyDictKeyEntry *b_entry = find(b, key, hash);
        if (b_entry == NULL) {
            Py_DECREF(key);
            Py_DECREF(a_val);
            if (PyErr_Occurred()) {
                return -1;
            }
            return 0;
        }

        PyObject *b_val = read_entry(b, b_keys, &b_entry->me_value);
        if (b_val == NULL) {
            Py_DECREF(key);
            Py_DECREF(a_val);
            return 0;
        }

        int cmp = PyObject_RichCompareBool(a_val, b_val, Py_EQ);
        Py_DECREF(key);
        Py_DECREF(a_val);
        Py_DECREF(b_val);

        if (cmp <= 0)  {
            // error or not equal
            return cmp;
        }

        // The keys object may be invalid because PyObject_RichCompareBool
        // can run arbitrary code.
        a_keys = _Py_atomic_load_ptr(&a->ma_keys);
    }
    return 1;
}

static PyObject *
dict_richcompare(PyObject *v, PyObject *w, int op)
{
    if (!PyDict_Check(v) || !PyDict_Check(w)) {
        return Py_NotImplemented;
    }
    else if (op == Py_EQ || op == Py_NE) {
        int cmp = dict_equal((PyDictObject *)v, (PyDictObject *)w);
        if (cmp < 0)
            return NULL;
        return (cmp == (op == Py_EQ)) ? Py_True : Py_False;
    }
    return Py_NotImplemented;
}

/*[clinic input]

@coexist
dict.__contains__

  key: object
  /

True if the dictionary has the specified key, else False.
[clinic start generated code]*/

static PyObject *
dict___contains__(PyDictObject *self, PyObject *key)
/*[clinic end generated code: output=a3d03db709ed6e6b input=fe1cb42ad831e820]*/
{
    int cmp = PyDict_Contains((PyObject *)self, key);
    if (cmp == 1) {
        Py_RETURN_TRUE;
    }
    else if (cmp == 0) {
        Py_RETURN_FALSE;
    }
    return NULL;
}

/*[clinic input]
dict.get

    key: object
    default: object = None
    /

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
dict_get_impl(PyDictObject *self, PyObject *key, PyObject *default_value)
/*[clinic end generated code: output=bba707729dee05bf input=279ddb5790b6b107]*/
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }
    PyObject *value = pydict_get(self, key, hash);
    if (value) {
        return value;
    }
    else if (PyErr_Occurred()) {
        return NULL;
    }
    Py_INCREF(default_value);
    return default_value;
}

PyObject *
_PyDict_SetDefault(PyObject *d, PyObject *key, PyObject *defaultobj,
                   int incref, int *is_insert)
{
    PyObject *value = NULL;

    if (!PyDict_Check(d)) {
        PyErr_BadInternalCall();
        return NULL;
    }

    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return NULL;
    }

    PyDictObject *mp = (PyDictObject *)d;
    Py_BEGIN_CRITICAL_SECTION(&mp->ma_mutex);
    PyDictKeyEntry *entry = find_or_prepare_insert(mp, key, hash, is_insert);
    if (!entry) {
        goto exit;
    }
    if (*is_insert) {
        MAINTAIN_TRACKING(mp, key, defaultobj);
        Py_INCREF(defaultobj);
        Py_INCREF(key);
        _Py_atomic_store_ptr_relaxed(&entry->me_key, key);
        _Py_atomic_store_ptr_relaxed(&entry->me_value, defaultobj);
        _Py_atomic_store_uint64_relaxed(&mp->ma_version_tag, DICT_NEXT_VERSION());
        if (incref) {
            Py_INCREF(defaultobj);
        }
        value = defaultobj;
    }
    else {
        value = entry->me_value;
        if (incref) {
            Py_INCREF(value);
        }
    }
    ASSERT_CONSISTENT(mp);

exit:
    Py_END_CRITICAL_SECTION;
    return value;
}

PyObject *
PyDict_SetDefault(PyObject *d, PyObject *key, PyObject *defaultobj)
{
    // NOTE: return value isn't thread-safe because it's a borrowed reference.
    int is_insert;
    return _PyDict_SetDefault(d, key, defaultobj, 0, &is_insert);
}

/*[clinic input]
dict.setdefault

    key: object
    default: object = None
    /

Insert key with a value of default if key is not in the dictionary.

Return the value for key if key is in the dictionary, else default.
[clinic start generated code]*/

static PyObject *
dict_setdefault_impl(PyDictObject *self, PyObject *key,
                     PyObject *default_value)
/*[clinic end generated code: output=f8c1101ebf69e220 input=0f063756e815fd9d]*/
{
    int is_insert;
    return _PyDict_SetDefault((PyObject *)self, key, default_value, 1, &is_insert);
}

static PyObject *
dict_clear(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    PyDict_Clear((PyObject *)mp);
    Py_RETURN_NONE;
}

/*[clinic input]
dict.pop

    key: object
    default: object = NULL
    /

D.pop(k[,d]) -> v, remove specified key and return the corresponding value.

If key is not found, default is returned if given, otherwise KeyError is raised
[clinic start generated code]*/

static PyObject *
dict_pop_impl(PyDictObject *self, PyObject *key, PyObject *default_value)
/*[clinic end generated code: output=3abb47b89f24c21c input=eeebec7812190348]*/
{
    return _PyDict_Pop((PyObject*)self, key, default_value);
}

/*[clinic input]
dict.popitem

Remove and return a (key, value) pair as a 2-tuple.

Pairs are returned in LIFO (last-in, first-out) order.
Raises KeyError if the dict is empty.
[clinic start generated code]*/

static PyObject *
dict_popitem_impl(PyDictObject *self)
/*[clinic end generated code: output=e65fcb04420d230d input=1c38a49f21f64941]*/
{
    /* Allocate the result tuple before checking the size.  Believe it
     * or not, this allocation could trigger a garbage collection which
     * could empty the dict, so if we checked the size first and that
     * happened, the result would be an infinite loop (searching for an
     * entry that no longer exists).  Note that the usual popitem()
     * idiom is "while d: k, v = d.popitem()". so needing to throw the
     * tuple away if the dict *is* empty isn't a significant
     * inefficiency -- possible, but unlikely in practice.
     */
    PyObject *res = PyTuple_New(2);
    if (res == NULL) {
        return NULL;
    }

    Py_BEGIN_CRITICAL_SECTION(&self->ma_mutex);
    if (self->ma_used == 0) {
        Py_CLEAR(res);
        PyErr_SetString(PyExc_KeyError, "popitem(): dictionary is empty");
        goto exit;
    }

    /* Pop last item */
    PyDictKeysObject *keys = self->ma_keys;
    PyDictKeyEntry *entry = NULL;
    while (!entry) {
        Py_ssize_t nentries = keys->dk_nentries;
        entry = entry_at(keys, nentries - 1);
        _Py_atomic_store_ssize_relaxed(&keys->dk_nentries, nentries - 1);
    }

    Py_INCREF(entry->me_key);
    Py_INCREF(entry->me_value);
    PyTuple_SET_ITEM(res, 0, entry->me_key);
    PyTuple_SET_ITEM(res, 1, entry->me_value);

    finish_erase(self, entry);

exit:
    Py_END_CRITICAL_SECTION;
    return res;
}

static int
dict_traverse(PyObject *op, visitproc visit, void *arg)
{
    PyDictObject *mp = (PyDictObject *)op;
    PyDictKeysObject *keys = mp->ma_keys;
    PyDictKeyEntry *entries = keys->dk_entries;
    for (Py_ssize_t i = 0, n = keys->dk_size; i < n; i++) {
        if (ctrl_is_full(keys->dk_ctrl[i])) {
            Py_VISIT(entries[i].me_value);
            if (keys->dk_type == DK_GENERIC) {
                Py_VISIT(entries[i].me_key);
            }
        }
    }
    return 0;
}

static int
dict_tp_clear(PyObject *op)
{
    PyDict_Clear(op);
    return 0;
}

static PyObject *dictiter_new(PyDictObject *, PyTypeObject *);

Py_ssize_t
_PyDict_SizeOf(PyDictObject *mp)
{
    Py_ssize_t res = _PyObject_SIZE(Py_TYPE(mp));
    if (mp->ma_keys != Py_EMPTY_KEYS) {
        /* If the dictionary is split, the keys portion is accounted-for
           in the type object. */
        res += _PyDict_KeysSize(mp->ma_keys);
    }
    return res;
}

Py_ssize_t
_PyDict_KeysSize(PyDictKeysObject *keys)
{
    Py_ssize_t size = keys->dk_size;
    Py_ssize_t usable = usable_fraction(size);
    Py_ssize_t ctrl_size = size < 16 ? 16 : size + 1;
    Py_ssize_t hash_size = keys->dk_type == DK_GENERIC ? size * sizeof(Py_hash_t) : 0;
    Py_ssize_t entry_size = sizeof(PyDictKeyEntry) * size;

    Py_ssize_t res = sizeof(PyDictKeysObject);
    res += ctrl_size;
    res += hash_size;
    res += entry_size;
    res += index_size(size) * (usable + 1);

    return res;
}

static PyObject *
dict_sizeof(PyDictObject *mp, PyObject *Py_UNUSED(ignored))
{
    return PyLong_FromSsize_t(_PyDict_SizeOf(mp));
}

static PyObject *
dict_or(PyObject *self, PyObject *other)
{
    if (!PyDict_Check(self) || !PyDict_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    PyObject *new = PyDict_Copy(self);
    if (new == NULL) {
        return NULL;
    }
    if (dict_update_arg(new, other)) {
        Py_DECREF(new);
        return NULL;
    }
    return new;
}

static PyObject *
dict_ior(PyObject *self, PyObject *other)
{
    if (dict_update_arg(self, other)) {
        return NULL;
    }
    Py_INCREF(self);
    return self;
}

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(sizeof__doc__,
"D.__sizeof__() -> size of D in memory, in bytes");

PyDoc_STRVAR(update__doc__,
"D.update([E, ]**F) -> None.  Update D from dict/iterable E and F.\n\
If E is present and has a .keys() method, then does:  for k in E: D[k] = E[k]\n\
If E is present and lacks a .keys() method, then does:  for k, v in E: D[k] = v\n\
In either case, this is followed by: for k in F:  D[k] = F[k]");

PyDoc_STRVAR(clear__doc__,
"D.clear() -> None.  Remove all items from D.");

PyDoc_STRVAR(copy__doc__,
"D.copy() -> a shallow copy of D");

/* Forward */
static PyObject *dictkeys_new(PyObject *, PyObject *);
static PyObject *dictitems_new(PyObject *, PyObject *);
static PyObject *dictvalues_new(PyObject *, PyObject *);

PyDoc_STRVAR(keys__doc__,
             "D.keys() -> a set-like object providing a view on D's keys");
PyDoc_STRVAR(items__doc__,
             "D.items() -> a set-like object providing a view on D's items");
PyDoc_STRVAR(values__doc__,
             "D.values() -> an object providing a view on D's values");

static PyMethodDef mapp_methods[] = {
    DICT___CONTAINS___METHODDEF
    {"__getitem__", (PyCFunction)(void(*)(void))dict_subscript,        METH_O | METH_COEXIST,
     getitem__doc__},
    {"__sizeof__",      (PyCFunction)(void(*)(void))dict_sizeof,       METH_NOARGS,
     sizeof__doc__},
    DICT_GET_METHODDEF
    DICT_SETDEFAULT_METHODDEF
    DICT_POP_METHODDEF
    DICT_POPITEM_METHODDEF
    {"keys",            dictkeys_new,                   METH_NOARGS,
    keys__doc__},
    {"items",           dictitems_new,                  METH_NOARGS,
    items__doc__},
    {"values",          dictvalues_new,                 METH_NOARGS,
    values__doc__},
    {"update",          (PyCFunction)(void(*)(void))dict_update, METH_VARARGS | METH_KEYWORDS,
     update__doc__},
    DICT_FROMKEYS_METHODDEF
    {"clear",           (PyCFunction)dict_clear,        METH_NOARGS,
     clear__doc__},
    {"copy",            (PyCFunction)dict_copy,         METH_NOARGS,
     copy__doc__},
    DICT___REVERSED___METHODDEF
    {"__class_getitem__", (PyCFunction)Py_GenericAlias, METH_O|METH_CLASS, PyDoc_STR("See PEP 585")},
    {NULL,              NULL}   /* sentinel */
};

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
int
PyDict_Contains(PyObject *op, PyObject *key)
{
    Py_hash_t hash = compute_hash(key);
    if (hash == -1) {
        return -1;
    }
    return _PyDict_Contains(op, key, hash);
}

/* Internal version of PyDict_Contains used when the hash value is already known */
int
_PyDict_Contains(PyObject *op, PyObject *key, Py_hash_t hash)
{
    PyDictKeyEntry *entry = find((PyDictObject *)op, key, hash);
    if (entry) {
        return 1;
    }
    else if (PyErr_Occurred()) {
        return -1;
    }
    return 0;
}

/* Hack to implement "key in dict" */
static PySequenceMethods dict_as_sequence = {
    0,                          /* sq_length */
    0,                          /* sq_concat */
    0,                          /* sq_repeat */
    0,                          /* sq_item */
    0,                          /* sq_slice */
    0,                          /* sq_ass_item */
    0,                          /* sq_ass_slice */
    PyDict_Contains,            /* sq_contains */
    0,                          /* sq_inplace_concat */
    0,                          /* sq_inplace_repeat */
};

static PyNumberMethods dict_as_number = {
    .nb_or = dict_or,
    .nb_inplace_or = dict_ior,
};

static PyObject *
dict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self;
    PyDictObject *d;

    assert(type != NULL && type->tp_alloc != NULL);
    self = type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;
    d = (PyDictObject *)self;

    /* The object has been implicitly tracked by tp_alloc */
    if (type == &PyDict_Type)
        _PyObject_GC_UNTRACK(d);

    d->ma_used = 0;
    d->ma_version_tag = DICT_NEXT_VERSION();
    d->ma_keys = new_keys_object(PyDict_MINSIZE, DK_UNICODE);
    if (d->ma_keys == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    ASSERT_CONSISTENT(d);
    return self;
}

static int
dict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    return dict_update_common(self, args, kwds, "dict");
}

static PyObject *
dict_vectorcall(PyObject *type, PyObject * const*args,
                size_t nargsf, PyObject *kwnames)
{
    assert(PyType_Check(type));
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (!_PyArg_CheckPositional("dict", nargs, 0, 1)) {
        return NULL;
    }

    PyObject *self = dict_new((PyTypeObject *)type, NULL, NULL);
    if (self == NULL) {
        return NULL;
    }
    if (nargs == 1) {
        if (dict_update_arg(self, args[0]) < 0) {
            Py_DECREF(self);
            return NULL;
        }
        args++;
    }
    if (kwnames != NULL) {
        for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
            if (PyDict_SetItem(self, PyTuple_GET_ITEM(kwnames, i), args[i]) < 0) {
                Py_DECREF(self);
                return NULL;
            }
        }
    }
    return self;
}

static PyObject *
dict_iter(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterKey_Type);
}

PyDoc_STRVAR(dictionary_doc,
"dict() -> new empty dictionary\n"
"dict(mapping) -> new dictionary initialized from a mapping object's\n"
"    (key, value) pairs\n"
"dict(iterable) -> new dictionary initialized as if via:\n"
"    d = {}\n"
"    for k, v in iterable:\n"
"        d[k] = v\n"
"dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  dict(one=1, two=2)");

PyTypeObject PyDict_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict",
    sizeof(PyDictObject),
    0,
    (destructor)dict_dealloc,                   /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dict_repr,                        /* tp_repr */
    &dict_as_number,                            /* tp_as_number */
    &dict_as_sequence,                          /* tp_as_sequence */
    &dict_as_mapping,                           /* tp_as_mapping */
    PyObject_HashNotImplemented,                /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_BASETYPE | Py_TPFLAGS_DICT_SUBCLASS,         /* tp_flags */
    dictionary_doc,                             /* tp_doc */
    dict_traverse,                              /* tp_traverse */
    dict_tp_clear,                              /* tp_clear */
    dict_richcompare,                           /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dict_iter,                     /* tp_iter */
    0,                                          /* tp_iternext */
    mapp_methods,                               /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    dict_init,                                  /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    dict_new,                                   /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
    .tp_vectorcall = dict_vectorcall,
};

PyObject *
_PyDict_GetItemId(PyObject *dp, struct _Py_Identifier *key)
{
    PyObject *kv;
    kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL) {
        PyErr_Clear();
        return NULL;
    }
    return PyDict_GetItem(dp, kv);
}

/* For backward compatibility with old dictionary interface */

PyObject *
PyDict_GetItemString(PyObject *v, const char *key)
{
    PyObject *kv, *rv;
    kv = PyUnicode_FromString(key);
    if (kv == NULL) {
        PyErr_Clear();
        return NULL;
    }
    rv = PyDict_GetItem(v, kv);
    Py_DECREF(kv);
    return rv;
}

int
_PyDict_SetItemId(PyObject *v, struct _Py_Identifier *key, PyObject *item)
{
    PyObject *kv;
    kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return -1;
    return PyDict_SetItem(v, kv, item);
}

int
PyDict_SetItemString(PyObject *v, const char *key, PyObject *item)
{
    PyObject *kv;
    int err;
    kv = PyUnicode_FromString(key);
    if (kv == NULL)
        return -1;
    PyUnicode_InternInPlace(&kv); /* XXX Should we really? */
    err = PyDict_SetItem(v, kv, item);
    Py_DECREF(kv);
    return err;
}

int
_PyDict_DelItemId(PyObject *v, _Py_Identifier *key)
{
    PyObject *kv = _PyUnicode_FromId(key); /* borrowed */
    if (kv == NULL)
        return -1;
    return PyDict_DelItem(v, kv);
}

int
PyDict_DelItemString(PyObject *v, const char *key)
{
    PyObject *kv;
    int err;
    kv = PyUnicode_FromString(key);
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

/* Dictionary iterator types */

typedef struct {
    PyObject_HEAD
    PyDictObject *di_dict; /* Set to NULL when iterator is exhausted */
    Py_ssize_t di_used;
    Py_ssize_t di_pos;
    PyObject* di_result; /* reusable result tuple for iteritems */
    Py_ssize_t len;
} dictiterobject;

static PyObject *
dictiter_new(PyDictObject *dict, PyTypeObject *itertype)
{
    dictiterobject *di;
    di = PyObject_GC_New(dictiterobject, itertype);
    if (di == NULL) {
        return NULL;
    }
    Py_INCREF(dict);
    di->di_dict = dict;
    di->di_used = dict->ma_used;
    di->len = dict->ma_used;
    if (itertype == &PyDictRevIterKey_Type ||
         itertype == &PyDictRevIterItem_Type ||
         itertype == &PyDictRevIterValue_Type) {
        di->di_pos = keys_nentries(dict->ma_keys) - 1;
    }
    else {
        di->di_pos = 0;
    }
    if (itertype == &PyDictIterItem_Type ||
        itertype == &PyDictRevIterItem_Type) {
        di->di_result = PyTuple_Pack(2, Py_None, Py_None);
        if (di->di_result == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else {
        di->di_result = NULL;
    }
    _PyObject_GC_TRACK(di);
    return (PyObject *)di;
}

static void
dictiter_dealloc(dictiterobject *di)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    _PyObject_GC_UNTRACK(di);
    Py_XDECREF(di->di_dict);
    Py_XDECREF(di->di_result);
    PyObject_GC_Del(di);
}

static int
dictiter_traverse(dictiterobject *di, visitproc visit, void *arg)
{
    Py_VISIT(di->di_dict);
    Py_VISIT(di->di_result);
    return 0;
}

static PyObject *
dictiter_len(dictiterobject *di, PyObject *Py_UNUSED(ignored))
{
    Py_ssize_t len = 0;
    if (di->di_dict != NULL && di->di_used == di->di_dict->ma_used)
        len = di->len;
    return PyLong_FromSize_t(len);
}

PyDoc_STRVAR(length_hint_doc,
             "Private method returning an estimate of len(list(it)).");

static PyObject *
dictiter_reduce(dictiterobject *di, PyObject *Py_UNUSED(ignored));

PyDoc_STRVAR(reduce_doc, "Return state information for pickling.");

static PyMethodDef dictiter_methods[] = {
    {"__length_hint__", (PyCFunction)(void(*)(void))dictiter_len, METH_NOARGS,
     length_hint_doc},
     {"__reduce__", (PyCFunction)(void(*)(void))dictiter_reduce, METH_NOARGS,
     reduce_doc},
    {NULL,              NULL}           /* sentinel */
};

static PyObject*
dictiter_iternextkey(dictiterobject *di)
{
    PyObject *key;
    Py_ssize_t i;
    PyDictKeysObject *k;
    PyDictObject *d = di->di_dict;

    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    k = _Py_atomic_load_ptr(&d->ma_keys);
    assert(i >= 0);
    PyDictKeyEntry *entry = next_entry(k, &i);
    if (entry == NULL) {
        return NULL;
    }
    key = read_entry(d, k, &entry->me_key);
    if (key == NULL || di->len == 0) {
        // We failed to read the key or found a key, but did not expect it
        Py_XDECREF(key);
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary keys changed during iteration");
        return NULL;
    }
    di->di_pos = i;
    di->len--;
    return key;
}

PyTypeObject PyDictIterKey_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_keyiterator",                         /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextkey,         /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject *
dictiter_iternextvalue(dictiterobject *di)
{
    PyObject *value;
    Py_ssize_t i;
    PyDictKeysObject *k;
    PyDictObject *d = di->di_dict;

    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    k = _Py_atomic_load_ptr(&d->ma_keys);
    assert(i >= 0);
    PyDictKeyEntry *entry = next_entry(k, &i);
    if (entry == NULL) {
        return NULL;
    }
    value = read_entry(d, k, &entry->me_value);
    if (value == NULL || di->len == 0) {
        // We failed to read the value or found a value, but did not expect it
        Py_XDECREF(value);
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary keys changed during iteration");
        return NULL;
    }
    di->di_pos = i;
    di->len--;
    return value;
}

PyTypeObject PyDictIterValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_valueiterator",                       /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextvalue,       /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject *
dictiter_iternextitem(dictiterobject *di)
{
    PyObject *key, *value, *result;
    Py_ssize_t i;
    PyDictKeysObject *k;
    PyDictObject *d = di->di_dict;

    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    k = _Py_atomic_load_ptr(&d->ma_keys);
    assert(i >= 0);
    PyDictKeyEntry *entry = next_entry(k, &i);
    if (entry == NULL) {
        return NULL;
    }
    key = read_entry(d, k, &entry->me_key);
    value = read_entry(d, k, &entry->me_value);
    // We found an element, but did not expect it
    if (key == NULL || value == NULL || di->len == 0) {
        Py_XDECREF(key);
        Py_XDECREF(value);
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary keys changed during iteration");
        return NULL;
    }
    di->di_pos = i;
    di->len--;
    result = di->di_result;
    if (Py_REFCNT(result) == 1) {
        PyObject *oldkey = PyTuple_GET_ITEM(result, 0);
        PyObject *oldvalue = PyTuple_GET_ITEM(result, 1);
        PyTuple_SET_ITEM(result, 0, key);  /* steals reference */
        PyTuple_SET_ITEM(result, 1, value);  /* steals reference */
        Py_INCREF(result);
        Py_DECREF(oldkey);
        Py_DECREF(oldvalue);
        // bpo-42536: The GC may have untracked this result tuple. Since we're
        // recycling it, make sure it's tracked again:
        if (!_PyObject_GC_IS_TRACKED(result)) {
            _PyObject_GC_TRACK(result);
        }
    }
    else {
        result = PyTuple_New(2);
        if (result == NULL) {
            Py_DECREF(key);
            Py_DECREF(value);
            return NULL;
        }
        PyTuple_SET_ITEM(result, 0, key);  /* steals reference */
        PyTuple_SET_ITEM(result, 1, value);  /* steals reference */
    }
    return result;
}

PyTypeObject PyDictIterItem_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_itemiterator",                        /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextitem,        /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};


/* dictreviter */

static PyObject *
dictreviter_iternext(dictiterobject *di)
{
    PyDictKeysObject *k;
    PyDictObject *d = di->di_dict;
    PyObject *key = NULL;
    PyObject *value = NULL;

    if (d == NULL) {
        return NULL;
    }
    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                         "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    k = _Py_atomic_load_ptr(&d->ma_keys);
    PyDictKeyEntry *entry_ptr = prev_entry(k, &di->di_pos);
    if (entry_ptr == NULL) {
        return NULL;
    }
    di->len--;

    if (Py_IS_TYPE(di, &PyDictRevIterKey_Type)) {
        key = read_entry(d, k, &entry_ptr->me_key);
        if (key == NULL) {
            goto fail;
        }
        return key;
    }
    else if (Py_IS_TYPE(di, &PyDictRevIterValue_Type)) {
        value = read_entry(d, k, &entry_ptr->me_value);
        if (value == NULL) {
            goto fail;
        }
        return value;
    }
    else if (Py_IS_TYPE(di, &PyDictRevIterItem_Type)) {
        key = read_entry(d, k, &entry_ptr->me_key);
        value = read_entry(d, k, &entry_ptr->me_value);
        if (key == NULL || value == NULL) {
            goto fail;
        }
        PyObject *result = PyTuple_New(2);
        if (result == NULL) {
            return NULL;
        }
        PyTuple_SET_ITEM(result, 0, key); /* steals reference */
        PyTuple_SET_ITEM(result, 1, value); /* steals reference */
        return result;
    }
    else {
        Py_UNREACHABLE();
        return NULL;
    }

fail:
    Py_XDECREF(key);
    Py_XDECREF(value);
    PyErr_SetString(PyExc_RuntimeError,
                    "dictionary keys changed during iteration");
    return NULL;
}

PyTypeObject PyDictRevIterKey_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_reversekeyiterator",
    sizeof(dictiterobject),
    .tp_dealloc = (destructor)dictiter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)dictiter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)dictreviter_iternext,
    .tp_methods = dictiter_methods
};


/*[clinic input]
dict.__reversed__

Return a reverse iterator over the dict keys.
[clinic start generated code]*/

static PyObject *
dict___reversed___impl(PyDictObject *self)
/*[clinic end generated code: output=e674483336d1ed51 input=23210ef3477d8c4d]*/
{
    assert (PyDict_Check(self));
    return dictiter_new(self, &PyDictRevIterKey_Type);
}

static PyObject *
dictiter_reduce(dictiterobject *di, PyObject *Py_UNUSED(ignored))
{
    _Py_IDENTIFIER(iter);
    /* copy the iterator state */
    dictiterobject tmp = *di;
    Py_XINCREF(tmp.di_dict);

    PyObject *list = PySequence_List((PyObject*)&tmp);
    Py_XDECREF(tmp.di_dict);
    if (list == NULL) {
        return NULL;
    }
    return Py_BuildValue("N(N)", _PyEval_GetBuiltinId(&PyId_iter), list);
}

PyTypeObject PyDictRevIterItem_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_reverseitemiterator",
    sizeof(dictiterobject),
    .tp_dealloc = (destructor)dictiter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)dictiter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)dictreviter_iternext,
    .tp_methods = dictiter_methods
};

PyTypeObject PyDictRevIterValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_reversevalueiterator",
    sizeof(dictiterobject),
    .tp_dealloc = (destructor)dictiter_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)dictiter_traverse,
    .tp_iter = PyObject_SelfIter,
    .tp_iternext = (iternextfunc)dictreviter_iternext,
    .tp_methods = dictiter_methods
};

/***********************************************/
/* View objects for keys(), items(), values(). */
/***********************************************/

/* The instance lay-out is the same for all three; but the type differs. */

static void
dictview_dealloc(_PyDictViewObject *dv)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    _PyObject_GC_UNTRACK(dv);
    Py_XDECREF(dv->dv_dict);
    PyObject_GC_Del(dv);
}

static int
dictview_traverse(_PyDictViewObject *dv, visitproc visit, void *arg)
{
    Py_VISIT(dv->dv_dict);
    return 0;
}

static Py_ssize_t
dictview_len(_PyDictViewObject *dv)
{
    return _Py_atomic_load_ssize(&dv->dv_dict->ma_used);
}

PyObject *
_PyDictView_New(PyObject *dict, PyTypeObject *type)
{
    _PyDictViewObject *dv;
    if (dict == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyDict_Check(dict)) {
        /* XXX Get rid of this restriction later */
        PyErr_Format(PyExc_TypeError,
                     "%s() requires a dict argument, not '%s'",
                     type->tp_name, Py_TYPE(dict)->tp_name);
        return NULL;
    }
    dv = PyObject_GC_New(_PyDictViewObject, type);
    if (dv == NULL)
        return NULL;
    Py_INCREF(dict);
    dv->dv_dict = (PyDictObject *)dict;
    _PyObject_GC_TRACK(dv);
    return (PyObject *)dv;
}

/* TODO(guido): The views objects are not complete:

 * support more set operations
 * support arbitrary mappings?
   - either these should be static or exported in dictobject.h
   - if public then they should probably be in builtins
*/

/* Return 1 if self is a subset of other, iterating over self;
   0 if not; -1 if an error occurred. */
static int
all_contained_in(PyObject *self, PyObject *other)
{
    PyObject *iter = PyObject_GetIter(self);
    int ok = 1;

    if (iter == NULL)
        return -1;
    for (;;) {
        PyObject *next = PyIter_Next(iter);
        if (next == NULL) {
            if (PyErr_Occurred())
                ok = -1;
            break;
        }
        ok = PySequence_Contains(other, next);
        Py_DECREF(next);
        if (ok <= 0)
            break;
    }
    Py_DECREF(iter);
    return ok;
}

static PyObject *
dictview_richcompare(PyObject *self, PyObject *other, int op)
{
    Py_ssize_t len_self, len_other;
    int ok;

    assert(self != NULL);
    assert(PyDictViewSet_Check(self));
    assert(other != NULL);

    if (!PyAnySet_Check(other) && !PyDictViewSet_Check(other))
        Py_RETURN_NOTIMPLEMENTED;

    len_self = PyObject_Size(self);
    if (len_self < 0)
        return NULL;
    len_other = PyObject_Size(other);
    if (len_other < 0)
        return NULL;

    ok = 0;
    switch(op) {

    case Py_NE:
    case Py_EQ:
        if (len_self == len_other)
            ok = all_contained_in(self, other);
        if (op == Py_NE && ok >= 0)
            ok = !ok;
        break;

    case Py_LT:
        if (len_self < len_other)
            ok = all_contained_in(self, other);
        break;

      case Py_LE:
          if (len_self <= len_other)
              ok = all_contained_in(self, other);
          break;

    case Py_GT:
        if (len_self > len_other)
            ok = all_contained_in(other, self);
        break;

    case Py_GE:
        if (len_self >= len_other)
            ok = all_contained_in(other, self);
        break;

    }
    if (ok < 0)
        return NULL;
    return ok ? Py_True : Py_False;
}

static PyObject *
dictview_repr(_PyDictViewObject *dv)
{
    PyObject *seq;
    PyObject *result = NULL;
    Py_ssize_t rc;

    rc = Py_ReprEnter((PyObject *)dv);
    if (rc != 0) {
        return rc > 0 ? PyUnicode_FromString("...") : NULL;
    }
    seq = PySequence_List((PyObject *)dv);
    if (seq == NULL) {
        goto Done;
    }
    result = PyUnicode_FromFormat("%s(%R)", Py_TYPE(dv)->tp_name, seq);
    Py_DECREF(seq);

Done:
    Py_ReprLeave((PyObject *)dv);
    return result;
}

/*** dict_keys ***/

static PyObject *
dictkeys_iter(_PyDictViewObject *dv)
{
    return dictiter_new(dv->dv_dict, &PyDictIterKey_Type);
}

static int
dictkeys_contains(_PyDictViewObject *dv, PyObject *obj)
{
    return PyDict_Contains((PyObject *)dv->dv_dict, obj);
}

static PySequenceMethods dictkeys_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictkeys_contains,      /* sq_contains */
};

// Create an set object from dictviews object.
// Returns a new reference.
// This utility function is used by set operations.
static PyObject*
dictviews_to_set(PyObject *self)
{
    PyObject *left = self;
    if (PyDictKeys_Check(self)) {
        // PySet_New() has fast path for the dict object.
        PyObject *dict = (PyObject *)((_PyDictViewObject *)self)->dv_dict;
        if (PyDict_CheckExact(dict)) {
            left = dict;
        }
    }
    return PySet_New(left);
}

static PyObject*
dictviews_sub(PyObject *self, PyObject *other)
{
    PyObject *result = dictviews_to_set(self);
    if (result == NULL) {
        return NULL;
    }

    _Py_IDENTIFIER(difference_update);
    PyObject *tmp = _PyObject_CallMethodIdOneArg(
            result, &PyId_difference_update, other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static int
dictitems_contains(_PyDictViewObject *dv, PyObject *obj);

PyObject *
_PyDictView_Intersect(PyObject* self, PyObject *other)
{
    PyObject *result;
    PyObject *it;
    PyObject *key;
    Py_ssize_t len_self;
    int rv;
    int (*dict_contains)(_PyDictViewObject *, PyObject *);

    /* Python interpreter swaps parameters when dict view
       is on right side of & */
    if (!PyDictViewSet_Check(self)) {
        PyObject *tmp = other;
        other = self;
        self = tmp;
    }

    len_self = dictview_len((_PyDictViewObject *)self);

    /* if other is a set and self is smaller than other,
       reuse set intersection logic */
    if (Py_IS_TYPE(other, &PySet_Type) && len_self <= PyObject_Size(other)) {
        _Py_IDENTIFIER(intersection);
        return _PyObject_CallMethodIdObjArgs(other, &PyId_intersection, self, NULL);
    }

    /* if other is another dict view, and it is bigger than self,
       swap them */
    if (PyDictViewSet_Check(other)) {
        Py_ssize_t len_other = dictview_len((_PyDictViewObject *)other);
        if (len_other > len_self) {
            PyObject *tmp = other;
            other = self;
            self = tmp;
        }
    }

    /* at this point, two things should be true
       1. self is a dictview
       2. if other is a dictview then it is smaller than self */
    result = PySet_New(NULL);
    if (result == NULL)
        return NULL;

    it = PyObject_GetIter(other);
    if (it == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    if (PyDictKeys_Check(self)) {
        dict_contains = dictkeys_contains;
    }
    /* else PyDictItems_Check(self) */
    else {
        dict_contains = dictitems_contains;
    }

    while ((key = PyIter_Next(it)) != NULL) {
        rv = dict_contains((_PyDictViewObject *)self, key);
        if (rv < 0) {
            goto error;
        }
        if (rv) {
            if (PySet_Add(result, key)) {
                goto error;
            }
        }
        Py_DECREF(key);
    }
    Py_DECREF(it);
    if (PyErr_Occurred()) {
        Py_DECREF(result);
        return NULL;
    }
    return result;

error:
    Py_DECREF(it);
    Py_DECREF(result);
    Py_DECREF(key);
    return NULL;
}

static PyObject*
dictviews_or(PyObject* self, PyObject *other)
{
    PyObject *result = dictviews_to_set(self);
    if (result == NULL) {
        return NULL;
    }

    if (_PySet_Update(result, other) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

static PyObject*
dictviews_xor(PyObject* self, PyObject *other)
{
    PyObject *result = dictviews_to_set(self);
    if (result == NULL) {
        return NULL;
    }

    _Py_IDENTIFIER(symmetric_difference_update);
    PyObject *tmp = _PyObject_CallMethodIdOneArg(
            result, &PyId_symmetric_difference_update, other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyNumberMethods dictviews_as_number = {
    0,                                  /*nb_add*/
    (binaryfunc)dictviews_sub,          /*nb_subtract*/
    0,                                  /*nb_multiply*/
    0,                                  /*nb_remainder*/
    0,                                  /*nb_divmod*/
    0,                                  /*nb_power*/
    0,                                  /*nb_negative*/
    0,                                  /*nb_positive*/
    0,                                  /*nb_absolute*/
    0,                                  /*nb_bool*/
    0,                                  /*nb_invert*/
    0,                                  /*nb_lshift*/
    0,                                  /*nb_rshift*/
    (binaryfunc)_PyDictView_Intersect,  /*nb_and*/
    (binaryfunc)dictviews_xor,          /*nb_xor*/
    (binaryfunc)dictviews_or,           /*nb_or*/
};

static PyObject*
dictviews_isdisjoint(PyObject *self, PyObject *other)
{
    PyObject *it;
    PyObject *item = NULL;

    if (self == other) {
        if (dictview_len((_PyDictViewObject *)self) == 0)
            Py_RETURN_TRUE;
        else
            Py_RETURN_FALSE;
    }

    /* Iterate over the shorter object (only if other is a set,
     * because PySequence_Contains may be expensive otherwise): */
    if (PyAnySet_Check(other) || PyDictViewSet_Check(other)) {
        Py_ssize_t len_self = dictview_len((_PyDictViewObject *)self);
        Py_ssize_t len_other = PyObject_Size(other);
        if (len_other == -1)
            return NULL;

        if ((len_other > len_self)) {
            PyObject *tmp = other;
            other = self;
            self = tmp;
        }
    }

    it = PyObject_GetIter(other);
    if (it == NULL)
        return NULL;

    while ((item = PyIter_Next(it)) != NULL) {
        int contains = PySequence_Contains(self, item);
        Py_DECREF(item);
        if (contains == -1) {
            Py_DECREF(it);
            return NULL;
        }

        if (contains) {
            Py_DECREF(it);
            Py_RETURN_FALSE;
        }
    }
    Py_DECREF(it);
    if (PyErr_Occurred())
        return NULL; /* PyIter_Next raised an exception. */
    Py_RETURN_TRUE;
}

PyDoc_STRVAR(isdisjoint_doc,
"Return True if the view and the given iterable have a null intersection.");

static PyObject* dictkeys_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored));

PyDoc_STRVAR(reversed_keys_doc,
"Return a reverse iterator over the dict keys.");

static PyMethodDef dictkeys_methods[] = {
    {"isdisjoint",      (PyCFunction)dictviews_isdisjoint,  METH_O,
     isdisjoint_doc},
    {"__reversed__",    (PyCFunction)(void(*)(void))dictkeys_reversed,    METH_NOARGS,
     reversed_keys_doc},
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictKeys_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_keys",                                /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictkeys_as_sequence,                      /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictkeys_iter,                 /* tp_iter */
    0,                                          /* tp_iternext */
    dictkeys_methods,                           /* tp_methods */
    0,
};

static PyObject *
dictkeys_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictKeys_Type);
}

static PyObject *
dictkeys_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored))
{
    return dictiter_new(dv->dv_dict, &PyDictRevIterKey_Type);
}

/*** dict_items ***/

static PyObject *
dictitems_iter(_PyDictViewObject *dv)
{
    return dictiter_new(dv->dv_dict, &PyDictIterItem_Type);
}

static int
dictitems_contains(_PyDictViewObject *dv, PyObject *obj)
{
    int result;
    PyObject *key, *value, *found;
    if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 2)
        return 0;
    key = PyTuple_GET_ITEM(obj, 0);
    value = PyTuple_GET_ITEM(obj, 1);
    found = PyDict_GetItemWithError2((PyObject *)dv->dv_dict, key);
    if (found == NULL) {
        if (PyErr_Occurred())
            return -1;
        return 0;
    }
    result = PyObject_RichCompareBool(found, value, Py_EQ);
    Py_DECREF(found);
    return result;
}

static PySequenceMethods dictitems_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictitems_contains,     /* sq_contains */
};

static PyObject* dictitems_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored));

PyDoc_STRVAR(reversed_items_doc,
"Return a reverse iterator over the dict items.");

static PyMethodDef dictitems_methods[] = {
    {"isdisjoint",      (PyCFunction)dictviews_isdisjoint,  METH_O,
     isdisjoint_doc},
    {"__reversed__",    (PyCFunction)dictitems_reversed,    METH_NOARGS,
     reversed_items_doc},
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictItems_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_items",                               /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictitems_as_sequence,                     /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictitems_iter,                /* tp_iter */
    0,                                          /* tp_iternext */
    dictitems_methods,                          /* tp_methods */
    0,
};

static PyObject *
dictitems_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictItems_Type);
}

static PyObject *
dictitems_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored))
{
    return dictiter_new(dv->dv_dict, &PyDictRevIterItem_Type);
}

/*** dict_values ***/

static PyObject *
dictvalues_iter(_PyDictViewObject *dv)
{
    return dictiter_new(dv->dv_dict, &PyDictIterValue_Type);
}

static PySequenceMethods dictvalues_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)0,                      /* sq_contains */
};

static PyObject* dictvalues_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored));

PyDoc_STRVAR(reversed_values_doc,
"Return a reverse iterator over the dict values.");

static PyMethodDef dictvalues_methods[] = {
    {"__reversed__",    (PyCFunction)dictvalues_reversed,    METH_NOARGS,
     reversed_values_doc},
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictValues_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_values",                              /* tp_name */
    sizeof(_PyDictViewObject),                  /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_vectorcall_offset */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_as_async */
    (reprfunc)dictview_repr,                    /* tp_repr */
    0,                                          /* tp_as_number */
    &dictvalues_as_sequence,                    /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictvalues_iter,               /* tp_iter */
    0,                                          /* tp_iternext */
    dictvalues_methods,                         /* tp_methods */
    0,
};

static PyObject *
dictvalues_new(PyObject *dict, PyObject *Py_UNUSED(ignored))
{
    return _PyDictView_New(dict, &PyDictValues_Type);
}

static PyObject *
dictvalues_reversed(_PyDictViewObject *dv, PyObject *Py_UNUSED(ignored))
{
    return dictiter_new(dv->dv_dict, &PyDictRevIterValue_Type);
}

static PyObject *
initialize_dict(PyObject **dictptr, PyTypeObject *tp)
{
    PyObject *dict = PyDict_New();
    if (!dict) {
        return NULL;
    }
    if (!_Py_atomic_compare_exchange_ptr(dictptr, NULL, dict)) {
        Py_DECREF(dict);
        dict = _Py_atomic_load_ptr(dictptr);
        assert(dict);
    }
    return dict;
}

PyObject *
PyObject_GenericGetDict(PyObject *obj, void *context)
{
    PyObject **dictptr = _PyObject_GetDictPtr(obj);
    if (dictptr == NULL) {
        PyErr_SetString(PyExc_AttributeError,
                        "This object has no __dict__");
        return NULL;
    }
    PyObject *dict = _Py_atomic_load_ptr_relaxed(dictptr);
    if (dict == NULL) {
        dict = initialize_dict(dictptr, Py_TYPE(obj));
    }
    Py_XINCREF(dict);
    return dict;
}

int
_PyObjectDict_SetItem(PyTypeObject *tp, PyObject **dictptr,
                      PyObject *key, PyObject *value)
{
    int res;

    assert(dictptr != NULL);
    PyObject *dict = _Py_atomic_load_ptr(dictptr);
    if (dict == NULL) {
        dict = initialize_dict(dictptr, tp);
        if (!dict) {
            return -1;
        }
    }

    if (value == NULL) {
        res = PyDict_DelItem(dict, key);
    } else {
        res = PyDict_SetItem(dict, key, value);
    }

    return res;
}
