#include "Python.h"
#include "pycore_interp.h"
#include "pycore_pyerrors.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"
#include "pycore_gc.h"
#include "pycore_object.h"

#include <stdbool.h>
#include "mimalloc.h"


/* Defined in tracemalloc.c */
extern void _PyMem_DumpTraceback(int fd, const void *ptr);


/* Python's malloc wrappers (see pymem.h) */

#undef  uint
#define uint    unsigned int    /* assuming >= 16 bits */

/* Forward declaration */
static void* _PyMem_DebugRawMalloc(void *ctx, size_t size);
static void* _PyMem_DebugRawCalloc(void *ctx, size_t nelem, size_t elsize);
static void* _PyMem_DebugRawRealloc(void *ctx, void *ptr, size_t size);
static void _PyMem_DebugRawFree(void *ctx, void *ptr);

static void* _PyMem_DebugMalloc(void *ctx, size_t size);
static void* _PyMem_DebugCalloc(void *ctx, size_t nelem, size_t elsize);
static void* _PyMem_DebugRealloc(void *ctx, void *ptr, size_t size);
static void _PyMem_DebugFree(void *ctx, void *p);

static void _PyObject_DebugDumpAddress(const void *p);
static void _PyMem_DebugCheckAddress(const char *func, char api_id, const void *p);

static void _PyMem_SetupDebugHooksDomain(PyMemAllocatorDomain domain);

#ifdef WITH_PYMALLOC

#ifdef MS_WINDOWS
#  include <windows.h>
#elif defined(HAVE_MMAP)
#  include <sys/mman.h>
#  ifdef MAP_ANONYMOUS
#    define ARENAS_USE_MMAP
#  endif
#endif

/* Forward declaration */
#endif


/* bpo-35053: Declare tracemalloc configuration here rather than
   Modules/_tracemalloc.c because _tracemalloc can be compiled as dynamic
   library, whereas _Py_NewReference() requires it. */
struct _PyTraceMalloc_Config _Py_tracemalloc_config = _PyTraceMalloc_Config_INIT;


static void *
_PyMem_RawMalloc(void *ctx, size_t size)
{
    /* PyMem_RawMalloc(0) means malloc(1). Some systems would return NULL
       for malloc(0), which would be treated as an error. Some platforms would
       return a pointer with no memory behind it, which would break pymalloc.
       To solve these problems, allocate an extra byte. */
    if (size == 0)
        size = 1;
    return malloc(size);
}

static void *
_PyMem_RawCalloc(void *ctx, size_t nelem, size_t elsize)
{
    /* PyMem_RawCalloc(0, 0) means calloc(1, 1). Some systems would return NULL
       for calloc(0, 0), which would be treated as an error. Some platforms
       would return a pointer with no memory behind it, which would break
       pymalloc.  To solve these problems, allocate an extra byte. */
    if (nelem == 0 || elsize == 0) {
        nelem = 1;
        elsize = 1;
    }
    return calloc(nelem, elsize);
}

static void *
_PyMem_RawRealloc(void *ctx, void *ptr, size_t size)
{
    if (size == 0)
        size = 1;
    return realloc(ptr, size);
}

static void
_PyMem_RawFree(void *ctx, void *ptr)
{
    free(ptr);
}


#ifdef MS_WINDOWS
static void *
_PyObject_ArenaVirtualAlloc(void *ctx, size_t size)
{
    return VirtualAlloc(NULL, size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void
_PyObject_ArenaVirtualFree(void *ctx, void *ptr, size_t size)
{
    VirtualFree(ptr, 0, MEM_RELEASE);
}

#elif defined(ARENAS_USE_MMAP)
static void *
_PyObject_ArenaMmap(void *ctx, size_t size)
{
    void *ptr;
    ptr = mmap(NULL, size, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    assert(ptr != NULL);
    return ptr;
}

static void
_PyObject_ArenaMunmap(void *ctx, void *ptr, size_t size)
{
    munmap(ptr, size);
}

#else
static void *
_PyObject_ArenaMalloc(void *ctx, size_t size)
{
    return malloc(size);
}

static void
_PyObject_ArenaFree(void *ctx, void *ptr, size_t size)
{
    free(ptr);
}
#endif

static void * _PyObject_Malloc(void *ctx, size_t nbytes);
static void * _PyObject_Calloc(void *ctx, size_t nelem, size_t elsize);
static void * _PyObject_Realloc(void *ctx, void *ptr, size_t nbytes);
static void _PyObject_Free(void *ctx, void *p);

static void * _GC_Malloc(void *ctx, size_t nbytes);
static void * _GC_Calloc(void *ctx, size_t nelem, size_t elsize);
static void * _GC_Realloc(void *ctx, void *ptr, size_t nbytes);

#define MALLOC_ALLOC {NULL, _PyMem_RawMalloc, _PyMem_RawCalloc, _PyMem_RawRealloc, _PyMem_RawFree}
#ifdef WITH_PYMALLOC
#  define PYMALLOC_ALLOC {NULL, _PyObject_Malloc, _PyObject_Calloc, _PyObject_Realloc, _PyObject_Free}
#endif

#define PYRAW_ALLOC MALLOC_ALLOC
#ifdef WITH_PYMALLOC
#  define PYOBJ_ALLOC PYMALLOC_ALLOC
#else
#  define PYOBJ_ALLOC MALLOC_ALLOC
#endif
#define PYMEM_ALLOC PYOBJ_ALLOC

typedef struct {
    /* We tag each block with an API ID in order to tag API violations */
    char api_id;
    PyMemAllocatorDomain domain;
} debug_alloc_api_t;
static struct {
    debug_alloc_api_t raw;
    debug_alloc_api_t mem;
    debug_alloc_api_t obj;
    debug_alloc_api_t gc;
} _PyMem_Debug = {
    {'r', PYMEM_DOMAIN_RAW},
    {'m', PYMEM_DOMAIN_MEM},
    {'o', PYMEM_DOMAIN_OBJ},
    {'g', PYMEM_DOMAIN_GC}
    };

#define PY_DEFAULT_ALLOCATORS \
    {NULL, _PyMem_RawMalloc, _PyMem_RawCalloc, _PyMem_RawRealloc, _PyMem_RawFree}, \
    {NULL, _PyObject_Malloc, _PyObject_Calloc, _PyObject_Realloc, _PyObject_Free}, \
    {NULL, _PyObject_Malloc, _PyObject_Calloc, _PyObject_Realloc, _PyObject_Free}, \
    {NULL, _GC_Malloc, _GC_Calloc, _GC_Realloc, _PyObject_Free}

#define PY_DEBUG_ALLOCATORS \
    {&_PyMem_Debug.raw, _PyMem_DebugRawMalloc, _PyMem_DebugRawCalloc, _PyMem_DebugRawRealloc, _PyMem_DebugRawFree}, \
    {&_PyMem_Debug.mem, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}, \
    {&_PyMem_Debug.obj, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}, \
    {&_PyMem_Debug.gc, _PyMem_DebugMalloc, _PyMem_DebugCalloc, _PyMem_DebugRealloc, _PyMem_DebugFree}

static const PyMemAllocatorEx default_allocators[4] = { PY_DEFAULT_ALLOCATORS };
static const PyMemAllocatorEx debug_allocators[4] = { PY_DEBUG_ALLOCATORS };

static PyMemAllocatorEx base_allocators[4] = { PY_DEFAULT_ALLOCATORS };

#ifdef Py_DEBUG
static PyMemAllocatorEx allocators[4] = { PY_DEBUG_ALLOCATORS };
static int _PyMem_debug_enabled = 1;
#else
static PyMemAllocatorEx allocators[4] = { PY_DEFAULT_ALLOCATORS };
static int _PyMem_debug_enabled = 0;
#endif

static int
pymem_set_default_allocator(PyMemAllocatorDomain domain, int debug,
                            PyMemAllocatorEx *old_alloc)
{
    if (old_alloc != NULL) {
        PyMem_GetAllocator(domain, old_alloc);
    }

    if (domain < 0 || domain >= PYMEM_DOMAIN_COUNT) {
        return -1;
    }

    PyMem_SetAllocator(domain, &default_allocators[domain]);
    if (debug) {
        _PyMem_SetupDebugHooksDomain(domain);
    }
    return 0;
}


int
_PyMem_SetDefaultAllocator(PyMemAllocatorDomain domain,
                           PyMemAllocatorEx *old_alloc)
{
#ifdef Py_DEBUG
    const int debug = 1;
#else
    const int debug = 0;
#endif
    if (domain == PYMEM_DOMAIN_GC) {
        _PyMem_debug_enabled = debug;
    }
    return pymem_set_default_allocator(domain, debug, old_alloc);
}


int
_PyMem_GetAllocatorName(const char *name, PyMemAllocatorName *allocator)
{
    if (name == NULL || *name == '\0') {
        /* PYTHONMALLOC is empty or is not set or ignored (-E/-I command line
           nameions): use default memory allocators */
        *allocator = PYMEM_ALLOCATOR_DEFAULT;
    }
    else if (strcmp(name, "default") == 0) {
        *allocator = PYMEM_ALLOCATOR_DEFAULT;
    }
    else if (strcmp(name, "debug") == 0) {
        *allocator = PYMEM_ALLOCATOR_DEBUG;
    }
#ifdef WITH_PYMALLOC
    else if (strcmp(name, "pymalloc") == 0) {
        *allocator = PYMEM_ALLOCATOR_PYMALLOC;
    }
    else if (strcmp(name, "pymalloc_debug") == 0) {
        *allocator = PYMEM_ALLOCATOR_PYMALLOC_DEBUG;
    }
#endif
    else if (strcmp(name, "malloc") == 0) {
        *allocator = PYMEM_ALLOCATOR_MALLOC;
    }
    else if (strcmp(name, "malloc_debug") == 0) {
        *allocator = PYMEM_ALLOCATOR_MALLOC_DEBUG;
    }
    else {
        /* unknown allocator */
        return -1;
    }
    return 0;
}


int
_PyMem_SetupAllocators(PyMemAllocatorName allocator)
{
    switch (allocator) {
    case PYMEM_ALLOCATOR_NOT_SET:
        /* do nothing */
        break;

    case PYMEM_ALLOCATOR_DEFAULT:
    case PYMEM_ALLOCATOR_PYMALLOC:
    case PYMEM_ALLOCATOR_MALLOC:
        memcpy(allocators, default_allocators, sizeof(allocators));
        _PyMem_debug_enabled = 0;
        break;

    case PYMEM_ALLOCATOR_DEBUG:
    case PYMEM_ALLOCATOR_PYMALLOC_DEBUG:
    case PYMEM_ALLOCATOR_MALLOC_DEBUG:
        memcpy(allocators, debug_allocators, sizeof(allocators));
        memcpy(base_allocators, default_allocators, sizeof(base_allocators));
        _PyMem_debug_enabled = 1;
        break;

    default:
        /* unknown allocator */
        return -1;
    }
    return 0;
}


static int
pymemallocator_eq(const PyMemAllocatorEx *a, const PyMemAllocatorEx *b)
{
    return (memcmp(a, b, sizeof(PyMemAllocatorEx)) == 0);
}

static int
pymemallocator_eq_all(const PyMemAllocatorEx a[], const PyMemAllocatorEx b[])
{
    for (int d = 0; d < PYMEM_DOMAIN_COUNT; d++) {
        if (!pymemallocator_eq(&a[d], &b[d])) {
            return 0;
        }
    }
    return 1;
}


const char*
_PyMem_GetCurrentAllocatorName(void)
{
    if (pymemallocator_eq_all(allocators, default_allocators)) {
        return "pymalloc";
    }
    if (pymemallocator_eq_all(allocators, debug_allocators)) {
        return "pymalloc_debug";
    }
    return NULL;
}


#undef MALLOC_ALLOC
#undef PYMALLOC_ALLOC
#undef PYRAW_ALLOC
#undef PYMEM_ALLOC
#undef PYOBJ_ALLOC
#undef PYDBGRAW_ALLOC
#undef PYDBGMEM_ALLOC
#undef PYDBGOBJ_ALLOC


static PyObjectArenaAllocator _PyObject_Arena = {NULL,
#ifdef MS_WINDOWS
    _PyObject_ArenaVirtualAlloc, _PyObject_ArenaVirtualFree
#elif defined(ARENAS_USE_MMAP)
    _PyObject_ArenaMmap, _PyObject_ArenaMunmap
#else
    _PyObject_ArenaMalloc, _PyObject_ArenaFree
#endif
    };

int
_PyMem_DebugEnabled(void)
{
    return _PyMem_debug_enabled;
}

static void
_PyMem_SetupDebugHooksDomain(PyMemAllocatorDomain domain)
{
    if (allocators[domain].malloc == debug_allocators[domain].malloc) {
        return;
    }

    PyMem_GetAllocator(domain, &base_allocators[domain]);
    PyMem_SetAllocator(domain, &debug_allocators[domain]);
}


void
PyMem_SetupDebugHooks(void)
{
    _PyMem_SetupDebugHooksDomain(PYMEM_DOMAIN_RAW);
    _PyMem_SetupDebugHooksDomain(PYMEM_DOMAIN_MEM);
    _PyMem_SetupDebugHooksDomain(PYMEM_DOMAIN_OBJ);
    _PyMem_SetupDebugHooksDomain(PYMEM_DOMAIN_GC);
}

void
PyMem_GetAllocator(PyMemAllocatorDomain domain, PyMemAllocatorEx *allocator)
{
    if (domain < 0 || domain >= PYMEM_DOMAIN_COUNT) {
        memset(allocator, 0, sizeof(*allocator));
    }
    else {
        *allocator = allocators[domain];
    }
}

void
PyMem_SetAllocator(PyMemAllocatorDomain domain, const PyMemAllocatorEx *allocator)
{
    if (domain < 0 || domain >= PYMEM_DOMAIN_COUNT) {
        return;
    }
    allocators[domain] = *allocator;
}

void
PyObject_GetArenaAllocator(PyObjectArenaAllocator *allocator)
{
    *allocator = _PyObject_Arena;
}

void
PyObject_SetArenaAllocator(PyObjectArenaAllocator *allocator)
{
    _PyObject_Arena = *allocator;
}

void *
PyMem_RawMalloc(size_t size)
{
    /*
     * Limit ourselves to PY_SSIZE_T_MAX bytes to prevent security holes.
     * Most python internals blindly use a signed Py_ssize_t to track
     * things without checking for overflows or negatives.
     * As size_t is unsigned, checking for size < 0 is not required.
     */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_RAW];
    return a->malloc(a->ctx, size);
}

void *
PyMem_RawCalloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_RAW];
    return a->calloc(a->ctx, nelem, elsize);
}

void*
PyMem_RawRealloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_RAW];
    return a->realloc(a->ctx, ptr, new_size);
}

void PyMem_RawFree(void *ptr)
{
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_RAW];
    a->free(a->ctx, ptr);
}


void *
PyMem_Malloc(size_t size)
{
    /* see PyMem_RawMalloc() */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_MEM];
    return a->malloc(a->ctx, size);
}

void *
PyMem_Calloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_MEM];
    return a->calloc(a->ctx, nelem, elsize);
}

void *
PyMem_Realloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_MEM];
    return a->realloc(a->ctx, ptr, new_size);
}

void
PyMem_Free(void *ptr)
{
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_MEM];
    a->free(a->ctx, ptr);
}


wchar_t*
_PyMem_RawWcsdup(const wchar_t *str)
{
    assert(str != NULL);

    size_t len = wcslen(str);
    if (len > (size_t)PY_SSIZE_T_MAX / sizeof(wchar_t) - 1) {
        return NULL;
    }

    size_t size = (len + 1) * sizeof(wchar_t);
    wchar_t *str2 = PyMem_RawMalloc(size);
    if (str2 == NULL) {
        return NULL;
    }

    memcpy(str2, str, size);
    return str2;
}

char *
_PyMem_RawStrdup(const char *str)
{
    assert(str != NULL);
    size_t size = strlen(str) + 1;
    char *copy = PyMem_RawMalloc(size);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, str, size);
    return copy;
}

char *
_PyMem_Strdup(const char *str)
{
    assert(str != NULL);
    size_t size = strlen(str) + 1;
    char *copy = PyMem_Malloc(size);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, str, size);
    return copy;
}

void* PyObject_Malloc(size_t size) {
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_OBJ];
    if (_PY_UNLIKELY(a->malloc != _PyObject_Malloc)) {
        return a->malloc(a->ctx, size);
    }
    return _PyObject_Malloc(NULL, size);
}

void* PyObject_Calloc(size_t nelem, size_t elsize) {
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_OBJ];
    if (_PY_UNLIKELY(a->calloc != _PyObject_Calloc)) {
        return a->calloc(a->ctx, nelem, elsize);
    }
    return _PyObject_Calloc(NULL, nelem, elsize);
}

void* PyObject_Realloc(void *ptr, size_t new_size) {
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_OBJ];
    if (_PY_UNLIKELY(a->realloc != _PyObject_Realloc)) {
        return a->realloc(a->ctx, ptr, new_size);
    }
    return _PyObject_Realloc(NULL, ptr, new_size);
}

void PyObject_Free(void *ptr) {
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_OBJ];
    if (_PY_UNLIKELY(a->free != _PyObject_Free)) {
        a->free(a->ctx, ptr);
        return;
    }
    _PyObject_Free(NULL, ptr);
}

static PyObject *
_PyObject_GC_Alloc(int use_calloc, size_t basicsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head)) {
        return _PyErr_NoMemory(tstate);
    }
    size_t size = sizeof(PyGC_Head) + basicsize;

    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_GC];
    PyGC_Head *g;
    if (use_calloc) {
        g = (PyGC_Head *)a->calloc(a->ctx, 1, size);
    }
    else {
        g = (PyGC_Head *)a->malloc(a->ctx, size);
    }
    if (g == NULL) {
        return _PyErr_NoMemory(tstate);
    }
    assert(((uintptr_t)g & 3) == 0);  // g must be aligned 4bytes boundary

    g->_gc_next = 0;
    g->_gc_prev = 0;

    PyObject *op = _PyObject_FROM_GC(g);
    return op;
}

PyObject *
_PyObject_GC_Malloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(0, basicsize);
}

PyObject *
_PyObject_GC_Calloc(size_t basicsize)
{
    return _PyObject_GC_Alloc(1, basicsize);
}

PyObject *
_PyObject_GC_New(PyTypeObject *tp)
{
    PyObject *op = _PyObject_GC_Malloc(_PyObject_SIZE(tp));
    if (op != NULL)
        op = PyObject_INIT(op, tp);
    return op;
}

PyVarObject *
_PyObject_GC_NewVar(PyTypeObject *tp, Py_ssize_t nitems)
{
    size_t size;
    PyVarObject *op;

    if (nitems < 0) {
        PyErr_BadInternalCall();
        return NULL;
    }
    size = _PyObject_VAR_SIZE(tp, nitems);
    op = (PyVarObject *) _PyObject_GC_Malloc(size);
    if (op != NULL)
        op = PyObject_INIT_VAR(op, tp, nitems);
    return op;
}

PyVarObject *
_PyObject_GC_Resize(PyVarObject *op, Py_ssize_t nitems)
{
    const size_t basicsize = _PyObject_VAR_SIZE(Py_TYPE(op), nitems);
    _PyObject_ASSERT((PyObject *)op, !_PyObject_GC_IS_TRACKED(op));
    if (basicsize > PY_SSIZE_T_MAX - sizeof(PyGC_Head)) {
        return (PyVarObject *)PyErr_NoMemory();
    }

    PyGC_Head *g = _Py_AS_GC(op);
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_GC];
    g = (PyGC_Head *)a->realloc(a->ctx, g, sizeof(PyGC_Head) + basicsize);
    if (g == NULL)
        return (PyVarObject *)PyErr_NoMemory();
    op = (PyVarObject *) _PyObject_FROM_GC(g);
    Py_SIZE(op) = nitems;
    return op;
}

void
PyObject_GC_Del(void *op)
{
    PyGC_Head *g = _Py_AS_GC(op);
    assert(g->_gc_next == 0);
    PyMemAllocatorEx *a = &allocators[PYMEM_DOMAIN_GC];
    a->free(a->ctx, g);
}


#ifdef WITH_PYMALLOC

static bool visit_blocks(
    const mi_heap_t* heap, const mi_heap_area_t* area,
    void* block, size_t block_size, void* arg)
{
    Py_ssize_t *allocated_blocks = (Py_ssize_t *)arg;
    *allocated_blocks += 1;
    return 1;
}

Py_ssize_t
_Py_GetAllocatedBlocks(void)
{
    Py_ssize_t allocated_blocks = 0;

    PyThreadState *tstate = _PyThreadState_GET();
    mi_heap_visit_blocks(tstate->heaps[mi_heap_tag_obj], 0, visit_blocks, &allocated_blocks);
    mi_heap_visit_blocks(tstate->heaps[mi_heap_tag_gc], 0, visit_blocks, &allocated_blocks);

    return allocated_blocks;
}

/*==========================================================================*/



// ------------------------------------------------------
// PyObject aliases
// ------------------------------------------------------


static inline void *
_PyObject_Malloc(void *ctx, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_malloc(tstate->heaps[mi_heap_tag_obj], nbytes);
}

static inline void *
_PyObject_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_calloc(tstate->heaps[mi_heap_tag_obj], nelem, elsize);
}

static inline void *
_PyObject_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_realloc(tstate->heaps[mi_heap_tag_obj], ptr, nbytes);
}

static inline void
_PyObject_Free(void *ctx, void *p)
{
    mi_free(p);
}

static inline void *
_GC_Malloc(void *ctx, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_malloc(tstate->heaps[mi_heap_tag_gc], nbytes);
}

static inline void *
_GC_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_calloc(tstate->heaps[mi_heap_tag_gc], nelem, elsize);
}

static inline void *
_GC_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_realloc(tstate->heaps[mi_heap_tag_gc], ptr, nbytes);
}

#else   /* ! WITH_PYMALLOC */

/*==========================================================================*/
/* pymalloc not enabled:  Redirect the entry points to malloc.  These will
 * only be used by extensions that are compiled with pymalloc enabled. */

Py_ssize_t
_Py_GetAllocatedBlocks(void)
{
    return 0;
}

#endif /* WITH_PYMALLOC */


/*==========================================================================*/
/* A x-platform debugging allocator.  This doesn't manage memory directly,
 * it wraps a real allocator, adding extra debugging info to the memory blocks.
 */

/* Uncomment this define to add the "serialno" field */
/* #define PYMEM_DEBUG_SERIALNO */

#ifdef PYMEM_DEBUG_SERIALNO
static size_t serialno = 0;     /* incremented on each debug {m,re}alloc */

/* serialno is always incremented via calling this routine.  The point is
 * to supply a single place to set a breakpoint.
 */
static void
bumpserialno(void)
{
    ++serialno;
}
#endif

#define SST SIZEOF_SIZE_T

#ifdef PYMEM_DEBUG_SERIALNO
#  define PYMEM_DEBUG_EXTRA_BYTES 4 * SST
#else
#  define PYMEM_DEBUG_EXTRA_BYTES 3 * SST
#endif

/* Write the size of a block to p. The size is stored
 * as (n<<1)|1 so that the LSB of the first words of an
 * allocated block is always set.
 */
static void
write_size_prefix(void *p, size_t n)
{
    size_t v = (n << 1)|1;
    memcpy(p, &v, sizeof(v));
}

/* Reads the size prefix. */
static size_t
read_size_prefix(const void *p)
{
    size_t value;
    memcpy(&value, p, sizeof(value));
    return value >> 1;
}

/* Let S = sizeof(size_t).  The debug malloc asks for 4 * S extra bytes and
   fills them with useful stuff, here calling the underlying malloc's result p:

p[0: S]
    Number of bytes originally asked for.  This is a size_t, big-endian (easier
    to read in a memory dump).
p[S]
    API ID.  See PEP 445.  This is a character, but seems undocumented.
p[S+1: 2*S]
    Copies of PYMEM_FORBIDDENBYTE.  Used to catch under- writes and reads.
p[2*S: 2*S+n]
    The requested memory, filled with copies of PYMEM_CLEANBYTE.
    Used to catch reference to uninitialized memory.
    &p[2*S] is returned.  Note that this is 8-byte aligned if pymalloc
    handled the request itself.
p[2*S+n: 2*S+n+S]
    Copies of PYMEM_FORBIDDENBYTE.  Used to catch over- writes and reads.
p[2*S+n+S: 2*S+n+2*S]
    A serial number, incremented by 1 on each call to _PyMem_DebugMalloc
    and _PyMem_DebugRealloc.
    This is a big-endian size_t.
    If "bad memory" is detected later, the serial number gives an
    excellent way to set a breakpoint on the next run, to capture the
    instant at which this block was passed out.

If PYMEM_DEBUG_SERIALNO is not defined (default), the debug malloc only asks
for 3 * S extra bytes, and omits the last serialno field.
*/

static void *
_PyMem_DebugRawAlloc(int use_calloc, void *ctx, size_t nbytes)
{
    debug_alloc_api_t *api = (debug_alloc_api_t *)ctx;
    uint8_t *p;           /* base address of malloc'ed pad block */
    uint8_t *data;        /* p + 2*SST == pointer to data bytes */
    uint8_t *tail;        /* data + nbytes == pointer to tail pad bytes */
    size_t total;         /* nbytes + PYMEM_DEBUG_EXTRA_BYTES */

    if (nbytes > (size_t)PY_SSIZE_T_MAX - PYMEM_DEBUG_EXTRA_BYTES) {
        /* integer overflow: can't represent total as a Py_ssize_t */
        return NULL;
    }
    total = nbytes + PYMEM_DEBUG_EXTRA_BYTES;

    /* Layout: [SSSS IFFF CCCC...CCCC FFFF NNNN]
                ^--- p    ^--- data   ^--- tail
       S: (nbytes<<1)|1 stored as size_t
       I: API identifier (1 byte)
       F: Forbidden bytes (size_t - 1 bytes before, size_t bytes after)
       C: Clean bytes used later to store actual data
       N: Serial number stored as size_t

       If PYMEM_DEBUG_SERIALNO is not defined (default), the last NNNN field
       is omitted. */

    PyMemAllocatorEx *base = &base_allocators[api->domain];
    if (use_calloc) {
        p = (uint8_t *)base->calloc(base->ctx, 1, total);
    }
    else {
        p = (uint8_t *)base->malloc(base->ctx, total);
    }
    if (p == NULL) {
        return NULL;
    }
    data = p + 2*SST;

#ifdef PYMEM_DEBUG_SERIALNO
    bumpserialno();
#endif

    /* at p, write size (SST bytes), id (1 byte), pad (SST-1 bytes) */
    write_size_prefix(p, nbytes);
    p[SST] = (uint8_t)api->api_id;
    memset(p + SST + 1, PYMEM_FORBIDDENBYTE, SST-1);

    if (nbytes > 0 && !use_calloc) {
        memset(data, PYMEM_CLEANBYTE, nbytes);
    }

    /* at tail, write pad (SST bytes) and serialno (SST bytes) */
    tail = data + nbytes;
    memset(tail, PYMEM_FORBIDDENBYTE, SST);
#ifdef PYMEM_DEBUG_SERIALNO
    write_size_prefix(tail + SST, serialno);
#endif

    return data;
}

static void *
_PyMem_DebugRawMalloc(void *ctx, size_t nbytes)
{
    return _PyMem_DebugRawAlloc(0, ctx, nbytes);
}

static void *
_PyMem_DebugRawCalloc(void *ctx, size_t nelem, size_t elsize)
{
    size_t nbytes;
    assert(elsize == 0 || nelem <= (size_t)PY_SSIZE_T_MAX / elsize);
    nbytes = nelem * elsize;
    return _PyMem_DebugRawAlloc(1, ctx, nbytes);
}


/* The debug free first checks the 2*SST bytes on each end for sanity (in
   particular, that the FORBIDDENBYTEs with the api ID are still intact).
   Then fills the original bytes with PYMEM_DEADBYTE.
   Then calls the underlying free.
*/
static void
_PyMem_DebugRawFree(void *ctx, void *p)
{
    /* PyMem_Free(NULL) has no effect */
    if (p == NULL) {
        return;
    }

    debug_alloc_api_t *api = (debug_alloc_api_t *)ctx;
    uint8_t *q = (uint8_t *)p - 2*SST;  /* address returned from malloc */
    size_t nbytes;

    _PyMem_DebugCheckAddress(__func__, api->api_id, p);
    nbytes = read_size_prefix(q);
    nbytes += PYMEM_DEBUG_EXTRA_BYTES;
    memset(q, PYMEM_DEADBYTE, nbytes);

    PyMemAllocatorEx *base = &base_allocators[api->domain];
    base->free(base->ctx, q);
}


static void *
_PyMem_DebugRawRealloc(void *ctx, void *p, size_t nbytes)
{
    if (p == NULL) {
        return _PyMem_DebugRawAlloc(0, ctx, nbytes);
    }

    debug_alloc_api_t *api = (debug_alloc_api_t *)ctx;
    uint8_t *head;        /* base address of malloc'ed pad block */
    uint8_t *data;        /* pointer to data bytes */
    uint8_t *r;
    uint8_t *tail;        /* data + nbytes == pointer to tail pad bytes */
    size_t total;         /* 2 * SST + nbytes + 2 * SST */
    size_t original_nbytes;
#define ERASED_SIZE 64
    uint8_t save[2*ERASED_SIZE];  /* A copy of erased bytes. */

    _PyMem_DebugCheckAddress(__func__, api->api_id, p);

    data = (uint8_t *)p;
    head = data - 2*SST;
    original_nbytes = read_size_prefix(head);
    if (nbytes > (size_t)PY_SSIZE_T_MAX - PYMEM_DEBUG_EXTRA_BYTES) {
        /* integer overflow: can't represent total as a Py_ssize_t */
        return NULL;
    }
    total = nbytes + PYMEM_DEBUG_EXTRA_BYTES;

    tail = data + original_nbytes;
#ifdef PYMEM_DEBUG_SERIALNO
    size_t block_serialno = read_size_prefix(tail + SST);
#endif
    /* Mark the header, the trailer, ERASED_SIZE bytes at the begin and
       ERASED_SIZE bytes at the end as dead and save the copy of erased bytes.
     */
    if (original_nbytes <= sizeof(save)) {
        memcpy(save, data, original_nbytes);
        memset(data - 2 * SST, PYMEM_DEADBYTE,
               original_nbytes + PYMEM_DEBUG_EXTRA_BYTES);
    }
    else {
        memcpy(save, data, ERASED_SIZE);
        memset(head, PYMEM_DEADBYTE, ERASED_SIZE + 2 * SST);
        memcpy(&save[ERASED_SIZE], tail - ERASED_SIZE, ERASED_SIZE);
        memset(tail - ERASED_SIZE, PYMEM_DEADBYTE,
               ERASED_SIZE + PYMEM_DEBUG_EXTRA_BYTES - 2 * SST);
    }

    /* Resize and add decorations. */
    PyMemAllocatorEx *base = &base_allocators[api->domain];
    r = (uint8_t *)base->realloc(base->ctx, head, total);
    if (r == NULL) {
        /* if realloc() failed: rewrite header and footer which have
           just been erased */
        nbytes = original_nbytes;
    }
    else {
        head = r;
#ifdef PYMEM_DEBUG_SERIALNO
        bumpserialno();
        block_serialno = serialno;
#endif
    }
    data = head + 2*SST;

    write_size_prefix(head, nbytes);
    head[SST] = (uint8_t)api->api_id;
    memset(head + SST + 1, PYMEM_FORBIDDENBYTE, SST-1);

    tail = data + nbytes;
    memset(tail, PYMEM_FORBIDDENBYTE, SST);
#ifdef PYMEM_DEBUG_SERIALNO
    write_size_prefix(tail + SST, block_serialno);
#endif

    /* Restore saved bytes. */
    if (original_nbytes <= sizeof(save)) {
        memcpy(data, save, Py_MIN(nbytes, original_nbytes));
    }
    else {
        size_t i = original_nbytes - ERASED_SIZE;
        memcpy(data, save, Py_MIN(nbytes, ERASED_SIZE));
        if (nbytes > i) {
            memcpy(data + i, &save[ERASED_SIZE],
                   Py_MIN(nbytes - i, ERASED_SIZE));
        }
    }

    if (r == NULL) {
        return NULL;
    }

    if (nbytes > original_nbytes) {
        /* growing: mark new extra memory clean */
        memset(data + original_nbytes, PYMEM_CLEANBYTE,
               nbytes - original_nbytes);
    }

    return data;
}

static inline void
_PyMem_DebugCheckGIL(const char *func)
{
    if (!PyGILState_Check()) {
        _Py_FatalErrorFunc(func,
                           "Python memory allocator called "
                           "without holding the GIL");
    }
}

static void *
_PyMem_DebugMalloc(void *ctx, size_t nbytes)
{
    _PyMem_DebugCheckGIL(__func__);
    return _PyMem_DebugRawMalloc(ctx, nbytes);
}

static void *
_PyMem_DebugCalloc(void *ctx, size_t nelem, size_t elsize)
{
    _PyMem_DebugCheckGIL(__func__);
    return _PyMem_DebugRawCalloc(ctx, nelem, elsize);
}


static void
_PyMem_DebugFree(void *ctx, void *ptr)
{
    _PyMem_DebugCheckGIL(__func__);
    _PyMem_DebugRawFree(ctx, ptr);
}


static void *
_PyMem_DebugRealloc(void *ctx, void *ptr, size_t nbytes)
{
    _PyMem_DebugCheckGIL(__func__);
    return _PyMem_DebugRawRealloc(ctx, ptr, nbytes);
}

/* Check the forbidden bytes on both ends of the memory allocated for p.
 * If anything is wrong, print info to stderr via _PyObject_DebugDumpAddress,
 * and call Py_FatalError to kill the program.
 * The API id, is also checked.
 */
static void
_PyMem_DebugCheckAddress(const char *func, char api, const void *p)
{
    assert(p != NULL);

    const uint8_t *q = (const uint8_t *)p;
    size_t nbytes;
    const uint8_t *tail;
    int i;
    char id;

    /* Check the API id */
    id = (char)q[-SST];
    if (id != api) {
        _PyObject_DebugDumpAddress(p);
        _Py_FatalErrorFormat(func,
                             "bad ID: Allocated using API '%c', "
                             "verified using API '%c'",
                             id, api);
    }

    /* Check the stuff at the start of p first:  if there's underwrite
     * corruption, the number-of-bytes field may be nuts, and checking
     * the tail could lead to a segfault then.
     */
    for (i = SST-1; i >= 1; --i) {
        if (*(q-i) != PYMEM_FORBIDDENBYTE) {
            _PyObject_DebugDumpAddress(p);
            _Py_FatalErrorFunc(func, "bad leading pad byte");
        }
    }

    nbytes = read_size_prefix(q - 2*SST);
    tail = q + nbytes;
    for (i = 0; i < SST; ++i) {
        if (tail[i] != PYMEM_FORBIDDENBYTE) {
            _PyObject_DebugDumpAddress(p);
            _Py_FatalErrorFunc(func, "bad trailing pad byte");
        }
    }
}

/* Display info to stderr about the memory block at p. */
static void
_PyObject_DebugDumpAddress(const void *p)
{
    const uint8_t *q = (const uint8_t *)p;
    const uint8_t *tail;
    size_t nbytes;
    int i;
    int ok;
    char id;

    fprintf(stderr, "Debug memory block at address p=%p:", p);
    if (p == NULL) {
        fprintf(stderr, "\n");
        return;
    }
    id = (char)q[-SST];
    fprintf(stderr, " API '%c'\n", id);

    nbytes = read_size_prefix(q - 2*SST);
    fprintf(stderr, "    %" PY_FORMAT_SIZE_T "u bytes originally "
                    "requested\n", nbytes);

    /* In case this is nuts, check the leading pad bytes first. */
    fprintf(stderr, "    The %d pad bytes at p-%d are ", SST-1, SST-1);
    ok = 1;
    for (i = 1; i <= SST-1; ++i) {
        if (*(q-i) != PYMEM_FORBIDDENBYTE) {
            ok = 0;
            break;
        }
    }
    if (ok)
        fputs("FORBIDDENBYTE, as expected.\n", stderr);
    else {
        fprintf(stderr, "not all FORBIDDENBYTE (0x%02x):\n",
            PYMEM_FORBIDDENBYTE);
        for (i = SST-1; i >= 1; --i) {
            const uint8_t byte = *(q-i);
            fprintf(stderr, "        at p-%d: 0x%02x", i, byte);
            if (byte != PYMEM_FORBIDDENBYTE)
                fputs(" *** OUCH", stderr);
            fputc('\n', stderr);
        }

        fputs("    Because memory is corrupted at the start, the "
              "count of bytes requested\n"
              "       may be bogus, and checking the trailing pad "
              "bytes may segfault.\n", stderr);
    }

    tail = q + nbytes;
    fprintf(stderr, "    The %d pad bytes at tail=%p are ", SST, (void *)tail);
    ok = 1;
    for (i = 0; i < SST; ++i) {
        if (tail[i] != PYMEM_FORBIDDENBYTE) {
            ok = 0;
            break;
        }
    }
    if (ok)
        fputs("FORBIDDENBYTE, as expected.\n", stderr);
    else {
        fprintf(stderr, "not all FORBIDDENBYTE (0x%02x):\n",
                PYMEM_FORBIDDENBYTE);
        for (i = 0; i < SST; ++i) {
            const uint8_t byte = tail[i];
            fprintf(stderr, "        at tail+%d: 0x%02x",
                    i, byte);
            if (byte != PYMEM_FORBIDDENBYTE)
                fputs(" *** OUCH", stderr);
            fputc('\n', stderr);
        }
    }

#ifdef PYMEM_DEBUG_SERIALNO
    size_t serial = read_size_prefix(tail + SST);
    fprintf(stderr, "    The block was made by call #%" PY_FORMAT_SIZE_T
                    "u to debug malloc/realloc.\n", serial);
#endif

    if (nbytes > 0) {
        i = 0;
        fputs("    Data at p:", stderr);
        /* print up to 8 bytes at the start */
        while (q < tail && i < 8) {
            fprintf(stderr, " %02x", *q);
            ++i;
            ++q;
        }
        /* and up to 8 at the end */
        if (q < tail) {
            if (tail - q > 8) {
                fputs(" ...", stderr);
                q = tail - 8;
            }
            while (q < tail) {
                fprintf(stderr, " %02x", *q);
                ++q;
            }
        }
        fputc('\n', stderr);
    }
    fputc('\n', stderr);

    fflush(stderr);
    _PyMem_DumpTraceback(fileno(stderr), p);
}


static size_t
printone(FILE *out, const char* msg, size_t value)
{
    int i, k;
    char buf[100];
    size_t origvalue = value;

    fputs(msg, out);
    for (i = (int)strlen(msg); i < 35; ++i)
        fputc(' ', out);
    fputc('=', out);

    /* Write the value with commas. */
    i = 22;
    buf[i--] = '\0';
    buf[i--] = '\n';
    k = 3;
    do {
        size_t nextvalue = value / 10;
        unsigned int digit = (unsigned int)(value - nextvalue * 10);
        value = nextvalue;
        buf[i--] = (char)(digit + '0');
        --k;
        if (k == 0 && value && i >= 0) {
            k = 3;
            buf[i--] = ',';
        }
    } while (value && i >= 0);

    while (i >= 0)
        buf[i--] = ' ';
    fputs(buf, out);

    return origvalue;
}

void
_PyDebugAllocatorStats(FILE *out,
                       const char *block_name, int num_blocks, size_t sizeof_block)
{
    char buf1[128];
    char buf2[128];
    PyOS_snprintf(buf1, sizeof(buf1),
                  "%d %ss * %" PY_FORMAT_SIZE_T "d bytes each",
                  num_blocks, block_name, sizeof_block);
    PyOS_snprintf(buf2, sizeof(buf2),
                  "%48s ", buf1);
    (void)printone(out, buf2, num_blocks * sizeof_block);
}


#ifdef WITH_PYMALLOC

/* Print summary info to "out" about the state of pymalloc's structures.
 * In Py_DEBUG mode, also perform some expensive internal consistency
 * checks.
 *
 * Return 0 if the memory debug hooks are not installed or no statistics was
 * written into out, return 1 otherwise.
 */
int
_PyObject_DebugMallocStats(FILE *out)
{
    return 0;
}

#endif /* #ifdef WITH_PYMALLOC */
