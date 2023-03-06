#include "Python.h"
#include "pycore_code.h"          // stats
#include "pycore_pystate.h"       // _PyInterpreterState_GET
#include "pycore_obmalloc.h"
#include "pycore_pymem.h"
#include "pycore_pymem_init.h"
#include "pycore_pyqueue.h"
#include "pycore_qsbr.h"

#include <stdlib.h>               // malloc()
#include <stdbool.h>
#include "mimalloc.h"


#undef  uint
#define uint pymem_uint


/* Defined in tracemalloc.c */
extern void _PyMem_DumpTraceback(int fd, const void *ptr);


/* Python's malloc wrappers (see pymem.h) */

static void _PyObject_DebugDumpAddress(const void *p);
static void _PyMem_DebugCheckAddress(const char *func, char api_id, const void *p);

static void _PyMem_SetupDebugHooksDomain(PyMemAllocatorDomain domain);


/***************************************/
/* low-level allocator implementations */
/***************************************/

/* the default raw allocator (wraps malloc) */

void *
_PyMem_RawMalloc(void *Py_UNUSED(ctx), size_t size)
{
    /* PyMem_RawMalloc(0) means malloc(1). Some systems would return NULL
       for malloc(0), which would be treated as an error. Some platforms would
       return a pointer with no memory behind it, which would break pymalloc.
       To solve these problems, allocate an extra byte. */
    if (size == 0)
        size = 1;
    return malloc(size);
}

void *
_PyMem_RawCalloc(void *Py_UNUSED(ctx), size_t nelem, size_t elsize)
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

void *
_PyMem_RawRealloc(void *Py_UNUSED(ctx), void *ptr, size_t size)
{
    if (size == 0)
        size = 1;
    return realloc(ptr, size);
}

void
_PyMem_RawFree(void *Py_UNUSED(ctx), void *ptr)
{
    free(ptr);
}

/* runtime default raw allocator */

void *
_PyMem_DefaultRawMalloc(size_t size)
{
#ifdef Py_DEBUG
    return _PyMem_DebugRawMalloc(&_PyRuntime.allocators.debug.raw, size);
#else
    return _PyMem_RawMalloc(NULL, size);
#endif
}

void *
_PyMem_DefaultRawCalloc(size_t nelem, size_t elsize)
{
#ifdef Py_DEBUG
    return _PyMem_DebugRawCalloc(&_PyRuntime.allocators.debug.raw, nelem, elsize);
#else
    return _PyMem_RawCalloc(NULL, nelem, elsize);
#endif

}

void *
_PyMem_DefaultRawRealloc(void *ptr, size_t size)
{
#ifdef Py_DEBUG
    return _PyMem_DebugRawRealloc(&_PyRuntime.allocators.debug.raw, ptr, size);
#else
    return _PyMem_RawRealloc(NULL, ptr, size);
#endif
}

void
_PyMem_DefaultRawFree(void *ptr)
{
#ifdef Py_DEBUG
    _PyMem_DebugRawFree(&_PyRuntime.allocators.debug.raw, ptr);
#else
    _PyMem_RawFree(NULL, ptr);
#endif
}

char *
_PyMem_DefaultRawStrdup(const char *str)
{
    assert(str != NULL);
    size_t size = strlen(str) + 1;
    char *copy = _PyMem_DefaultRawMalloc(size);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, str, size);
    return copy;
}

wchar_t*
_PyMem_DefaultRawWcsdup(const wchar_t *str)
{
    assert(str != NULL);

    size_t len = wcslen(str);
    if (len > (size_t)PY_SSIZE_T_MAX / sizeof(wchar_t) - 1) {
        return NULL;
    }

    size_t size = (len + 1) * sizeof(wchar_t);
    wchar_t *str2 = _PyMem_DefaultRawMalloc(size);
    if (str2 == NULL) {
        return NULL;
    }

    memcpy(str2, str, size);
    return str2;
}

#define MALLOC_ALLOC {NULL, _PyMem_RawMalloc, _PyMem_RawCalloc, _PyMem_RawRealloc, _PyMem_RawFree}

/* the default object allocator */

// The actual implementation is further down.

/* the default debug allocators */

// The actual implementation is further down.

void* _PyMem_DebugRawMalloc(void *ctx, size_t size);
void* _PyMem_DebugRawCalloc(void *ctx, size_t nelem, size_t elsize);
void* _PyMem_DebugRawRealloc(void *ctx, void *ptr, size_t size);
void _PyMem_DebugRawFree(void *ctx, void *ptr);

void* _PyMem_DebugMalloc(void *ctx, size_t size);
void* _PyMem_DebugCalloc(void *ctx, size_t nelem, size_t elsize);
void* _PyMem_DebugRealloc(void *ctx, void *ptr, size_t size);
void _PyMem_DebugFree(void *ctx, void *p);

/* the low-level virtual memory allocator */

#ifdef WITH_PYMALLOC
#  ifdef MS_WINDOWS
#    include <windows.h>
#  elif defined(HAVE_MMAP)
#    include <sys/mman.h>
#    ifdef MAP_ANONYMOUS
#      define ARENAS_USE_MMAP
#    endif
#  endif
#endif

void *
_PyMem_ArenaAlloc(void *Py_UNUSED(ctx), size_t size)
{
#ifdef MS_WINDOWS
    return VirtualAlloc(NULL, size,
                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#elif defined(ARENAS_USE_MMAP)
    void *ptr;
    ptr = mmap(NULL, size, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    assert(ptr != NULL);
    return ptr;
#else
    return malloc(size);
#endif
}

void
_PyMem_ArenaFree(void *Py_UNUSED(ctx), void *ptr,
#if defined(ARENAS_USE_MMAP)
    size_t size
#else
    size_t Py_UNUSED(size)
#endif
)
{
#ifdef MS_WINDOWS
    VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(ARENAS_USE_MMAP)
    munmap(ptr, size);
#else
    free(ptr);
#endif
}

/*******************************************/
/* end low-level allocator implementations */
/*******************************************/


#define _PyMem_Raw (_PyRuntime.allocators.standard.raw)
#define _PyMem (_PyRuntime.allocators.standard.mem)
#define _PyObject (_PyRuntime.allocators.standard.obj)
#define _PyGC (_PyRuntime.allocators.standard.gc)
#define _PyMem_Debug (_PyRuntime.allocators.debug)
#define _PyObject_Arena (_PyRuntime.allocators.obj_arena)

#ifdef Py_DEBUG
static int _PyMem_debug_enabled = 1;
#else
static int _PyMem_debug_enabled = 0;
#endif

static int
pymem_set_default_allocator(PyMemAllocatorDomain domain, int debug,
                            PyMemAllocatorEx *old_alloc)
{
    if (old_alloc != NULL) {
        PyMem_GetAllocator(domain, old_alloc);
    }


    PyMemAllocatorEx new_alloc;
    switch(domain)
    {
    case PYMEM_DOMAIN_RAW:
        new_alloc = (PyMemAllocatorEx)PYRAW_ALLOC;
        break;
    case PYMEM_DOMAIN_MEM:
        new_alloc = (PyMemAllocatorEx)PYMEM_ALLOC;
        break;
    case PYMEM_DOMAIN_OBJ:
        new_alloc = (PyMemAllocatorEx)PYOBJ_ALLOC;
        break;
    case PYMEM_DOMAIN_GC:
        new_alloc = (PyMemAllocatorEx)PYGC_ALLOC;
        break;
    default:
        /* unknown domain */
        return -1;
    }
    PyMem_SetAllocator(domain, &new_alloc);
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
        (void)_PyMem_SetDefaultAllocator(PYMEM_DOMAIN_RAW, NULL);
        (void)_PyMem_SetDefaultAllocator(PYMEM_DOMAIN_MEM, NULL);
        (void)_PyMem_SetDefaultAllocator(PYMEM_DOMAIN_OBJ, NULL);
        (void)_PyMem_SetDefaultAllocator(PYMEM_DOMAIN_GC, NULL);
        break;

    case PYMEM_ALLOCATOR_DEBUG:
        (void)pymem_set_default_allocator(PYMEM_DOMAIN_RAW, 1, NULL);
        (void)pymem_set_default_allocator(PYMEM_DOMAIN_MEM, 1, NULL);
        (void)pymem_set_default_allocator(PYMEM_DOMAIN_OBJ, 1, NULL);
        (void)pymem_set_default_allocator(PYMEM_DOMAIN_GC, 1, NULL);
        break;

#ifdef WITH_PYMALLOC
    case PYMEM_ALLOCATOR_PYMALLOC:
    case PYMEM_ALLOCATOR_PYMALLOC_DEBUG:
    {
        PyMemAllocatorEx malloc_alloc = MALLOC_ALLOC;
        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &malloc_alloc);

        PyMemAllocatorEx pymem = PYMEM_ALLOC;
        PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &pymem);

        PyMemAllocatorEx pyobj = PYOBJ_ALLOC;
        PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &pyobj);

        PyMemAllocatorEx pygc = PYGC_ALLOC;
        PyMem_SetAllocator(PYMEM_DOMAIN_GC, &pygc);

        _PyMem_debug_enabled = (allocator == PYMEM_ALLOCATOR_PYMALLOC_DEBUG);
        if (_PyMem_debug_enabled) {
            PyMem_SetupDebugHooks();
        }
        break;
    }
#endif

    case PYMEM_ALLOCATOR_MALLOC:
    case PYMEM_ALLOCATOR_MALLOC_DEBUG:
    {
        PyMemAllocatorEx malloc_alloc = MALLOC_ALLOC;
        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &malloc_alloc);
        PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &malloc_alloc);
        PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &malloc_alloc);

        if (allocator == PYMEM_ALLOCATOR_MALLOC_DEBUG) {
            PyMem_SetupDebugHooks();
        }
        break;
    }

    default:
        /* unknown allocator */
        return -1;
    }
    return 0;
}


static int
pymemallocator_eq(PyMemAllocatorEx *a, PyMemAllocatorEx *b)
{
    return (memcmp(a, b, sizeof(PyMemAllocatorEx)) == 0);
}


const char*
_PyMem_GetCurrentAllocatorName(void)
{
    PyMemAllocatorEx malloc_alloc = MALLOC_ALLOC;

    if (pymemallocator_eq(&_PyMem_Raw, &malloc_alloc) &&
        pymemallocator_eq(&_PyMem, &malloc_alloc) &&
        pymemallocator_eq(&_PyObject, &malloc_alloc))
    {
        return "malloc";
    }

    PyMemAllocatorEx pymem = PYMEM_ALLOC;
    PyMemAllocatorEx pyobj = PYOBJ_ALLOC;
    PyMemAllocatorEx pygc = PYGC_ALLOC;
    if (pymemallocator_eq(&_PyMem_Raw, &malloc_alloc) &&
        pymemallocator_eq(&_PyMem, &pymem) &&
        pymemallocator_eq(&_PyObject, &pyobj) &&
        pymemallocator_eq(&_PyGC, &pygc))
    {
#ifdef WITH_PYMALLOC
        return "pymalloc";
#else
        return "malloc";
#endif
    }

    PyMemAllocatorEx dbg_raw = PYDBGRAW_ALLOC(_PyRuntime);
    PyMemAllocatorEx dbg_mem = PYDBGMEM_ALLOC(_PyRuntime);
    PyMemAllocatorEx dbg_obj = PYDBGOBJ_ALLOC(_PyRuntime);
    PyMemAllocatorEx dbg_gc = PYDBGGC_ALLOC(_PyRuntime);

    if (pymemallocator_eq(&_PyMem_Raw, &dbg_raw) &&
        pymemallocator_eq(&_PyMem, &dbg_mem) &&
        pymemallocator_eq(&_PyObject, &dbg_obj) &&
        pymemallocator_eq(&_PyGC, &dbg_gc))
    {
        /* Debug hooks installed */
        if (pymemallocator_eq(&_PyMem_Debug.raw.alloc, &malloc_alloc) &&
            pymemallocator_eq(&_PyMem_Debug.mem.alloc, &malloc_alloc) &&
            pymemallocator_eq(&_PyMem_Debug.obj.alloc, &malloc_alloc))
        {
            return "malloc_debug";
        }
        if (pymemallocator_eq(&_PyMem_Debug.raw.alloc, &malloc_alloc) &&
            pymemallocator_eq(&_PyMem_Debug.mem.alloc, &pymem) &&
            pymemallocator_eq(&_PyMem_Debug.obj.alloc, &pyobj) &&
            pymemallocator_eq(&_PyMem_Debug.gc.alloc, &pygc))
        {
#ifdef WITH_PYMALLOC
            return "pymalloc_debug";
#else
            return "malloc_debug";
#endif
        }
    }
    return NULL;
}


int
_PyMem_DebugEnabled(void)
{
    return _PyMem_debug_enabled;
}


static void
_PyMem_SetupDebugHooksDomain(PyMemAllocatorDomain domain)
{
    PyMemAllocatorEx alloc;

    if (domain == PYMEM_DOMAIN_RAW) {
        if (_PyMem_Raw.malloc == _PyMem_DebugRawMalloc) {
            return;
        }

        PyMem_GetAllocator(PYMEM_DOMAIN_RAW, &_PyMem_Debug.raw.alloc);
        alloc.ctx = &_PyMem_Debug.raw;
        alloc.malloc = _PyMem_DebugRawMalloc;
        alloc.calloc = _PyMem_DebugRawCalloc;
        alloc.realloc = _PyMem_DebugRawRealloc;
        alloc.free = _PyMem_DebugRawFree;
        PyMem_SetAllocator(PYMEM_DOMAIN_RAW, &alloc);
    }
    else if (domain == PYMEM_DOMAIN_MEM) {
        if (_PyMem.malloc == _PyMem_DebugMalloc) {
            return;
        }

        PyMem_GetAllocator(PYMEM_DOMAIN_MEM, &_PyMem_Debug.mem.alloc);
        alloc.ctx = &_PyMem_Debug.mem;
        alloc.malloc = _PyMem_DebugMalloc;
        alloc.calloc = _PyMem_DebugCalloc;
        alloc.realloc = _PyMem_DebugRealloc;
        alloc.free = _PyMem_DebugFree;
        PyMem_SetAllocator(PYMEM_DOMAIN_MEM, &alloc);
    }
    else if (domain == PYMEM_DOMAIN_OBJ)  {
        if (_PyObject.malloc == _PyMem_DebugMalloc) {
            return;
        }

        PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &_PyMem_Debug.obj.alloc);
        alloc.ctx = &_PyMem_Debug.obj;
        alloc.malloc = _PyMem_DebugMalloc;
        alloc.calloc = _PyMem_DebugCalloc;
        alloc.realloc = _PyMem_DebugRealloc;
        alloc.free = _PyMem_DebugFree;
        PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &alloc);
    }
    else if (domain == PYMEM_DOMAIN_GC)  {
        if (_PyGC.malloc == _PyMem_DebugMalloc) {
            return;
        }

        PyMem_GetAllocator(PYMEM_DOMAIN_GC, &_PyMem_Debug.gc.alloc);
        alloc.ctx = &_PyMem_Debug.gc;
        alloc.malloc = _PyMem_DebugMalloc;
        alloc.calloc = _PyMem_DebugCalloc;
        alloc.realloc = _PyMem_DebugRealloc;
        alloc.free = _PyMem_DebugFree;
        _PyMem_debug_enabled = 1;
        PyMem_SetAllocator(PYMEM_DOMAIN_GC, &alloc);
    }
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
    switch(domain)
    {
    case PYMEM_DOMAIN_RAW: *allocator = _PyMem_Raw; break;
    case PYMEM_DOMAIN_MEM: *allocator = _PyMem; break;
    case PYMEM_DOMAIN_OBJ: *allocator = _PyObject; break;
    case PYMEM_DOMAIN_GC: *allocator = _PyGC; break;
    default:
        /* unknown domain: set all attributes to NULL */
        allocator->ctx = NULL;
        allocator->malloc = NULL;
        allocator->calloc = NULL;
        allocator->realloc = NULL;
        allocator->free = NULL;
    }
}

void
PyMem_SetAllocator(PyMemAllocatorDomain domain, const PyMemAllocatorEx *allocator)
{
    switch(domain)
    {
    case PYMEM_DOMAIN_RAW: _PyMem_Raw = *allocator; break;
    case PYMEM_DOMAIN_MEM: _PyMem = *allocator; break;
    case PYMEM_DOMAIN_OBJ: _PyObject = *allocator; break;
    case PYMEM_DOMAIN_GC: _PyGC = *allocator; break;
    default: break;
    /* ignore unknown domain */
    }
}

void
PyObject_GetArenaAllocator(PyObjectArenaAllocator *allocator)
{
    *allocator = _PyObject_Arena;
}

void *
_PyObject_VirtualAlloc(size_t size)
{
    return _PyObject_Arena.alloc(_PyObject_Arena.ctx, size);
}

void
_PyObject_VirtualFree(void *obj, size_t size)
{
    _PyObject_Arena.free(_PyObject_Arena.ctx, obj, size);
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
    return _PyMem_Raw.malloc(_PyMem_Raw.ctx, size);
}

void *
PyMem_RawCalloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    return _PyMem_Raw.calloc(_PyMem_Raw.ctx, nelem, elsize);
}

void*
PyMem_RawRealloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMem_Raw.realloc(_PyMem_Raw.ctx, ptr, new_size);
}

void PyMem_RawFree(void *ptr)
{
    _PyMem_Raw.free(_PyMem_Raw.ctx, ptr);
}


void *
PyMem_Malloc(size_t size)
{
    /* see PyMem_RawMalloc() */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    OBJECT_STAT_INC_COND(allocations512, size < 512);
    OBJECT_STAT_INC_COND(allocations4k, size >= 512 && size < 4094);
    OBJECT_STAT_INC_COND(allocations_big, size >= 4094);
    OBJECT_STAT_INC(allocations);
    return _PyMem.malloc(_PyMem.ctx, size);
}

void *
PyMem_Calloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    OBJECT_STAT_INC_COND(allocations512, elsize < 512);
    OBJECT_STAT_INC_COND(allocations4k, elsize >= 512 && elsize < 4094);
    OBJECT_STAT_INC_COND(allocations_big, elsize >= 4094);
    OBJECT_STAT_INC(allocations);
    return _PyMem.calloc(_PyMem.ctx, nelem, elsize);
}

void *
PyMem_Realloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyMem.realloc(_PyMem.ctx, ptr, new_size);
}

void
PyMem_Free(void *ptr)
{
    OBJECT_STAT_INC(frees);
    _PyMem.free(_PyMem.ctx, ptr);
}


typedef struct {
    void *ptr;
    uint64_t seq;
} _PyMem_WorkItem;

#define PY_MEM_WORK_ITEMS 127

typedef struct _PyMemWork {
    struct _Py_queue_node node;
    unsigned int first;
    unsigned int size;
    _PyMem_WorkItem items[PY_MEM_WORK_ITEMS];
} _PyMem_WorkBuf;

void
_PyMem_FreeQsbr(void *ptr)
{
    PyThreadState *tstate = _PyThreadState_GET();

    // Try to get an non-full workbuf
    _PyMem_WorkBuf *work = NULL;
    if (!_Py_queue_is_empty(&tstate->mem_work)) {
        work = _Py_queue_last(&tstate->mem_work, _PyMem_WorkBuf, node);
        if (work->size == PY_MEM_WORK_ITEMS) {
            work = NULL;
        }
    }

    if (work == NULL) {
        work = PyMem_RawMalloc(sizeof(_PyMem_WorkBuf));
        if (work == NULL) {
            Py_FatalError("out of memory (in _PyMem_FreeQsbr)");
        }
        work->first = work->size = 0;
        _Py_queue_enqeue(&tstate->mem_work, &work->node);
    }

    PyThreadStateImpl *tstate_impl = (PyThreadStateImpl *)tstate;
    work->items[work->size].ptr = ptr;
    work->items[work->size].seq = _Py_qsbr_deferred_advance(tstate_impl->qsbr);
    work->size++;

    if (work->size == PY_MEM_WORK_ITEMS) {
        // Now seems like a good time to check for any memory that can be freed.
        _PyMem_QsbrPoll(tstate);
    }
}

static int
_PyMem_ProcessQueue(struct _Py_queue_head *queue, struct qsbr *qsbr, bool keep_empty)
{
    while (!_Py_queue_is_empty(queue)) {
        _PyMem_WorkBuf *work = _Py_queue_first(queue, _PyMem_WorkBuf, node);
        if (work->size == 0 && keep_empty) {
            return 0;
        }
        while (work->first < work->size) {
            _PyMem_WorkItem *item = &work->items[work->first];
            if (!_Py_qsbr_poll(qsbr, item->seq)) {
                return 1;
            }
            PyMem_Free(item->ptr);
            work->first++;
        }

        // Remove the empty work buffer
        _Py_queue_dequeue(queue);

        // If the queue doesn't have an empty work buffer, stick this
        // one at the end of the queue.  Otherwise, free it.
        if (keep_empty && _Py_queue_is_empty(queue)) {
            work->first = work->size = 0;
            _Py_queue_enqeue(queue, &work->node);
            return 0;
        }
        else if (keep_empty && _Py_queue_last(queue, _PyMem_WorkBuf, node)->size == 0) {
            work->first = work->size = 0;
            _Py_queue_enqeue(queue, &work->node);
        }
        else {
            PyMem_RawFree(work);
        }
    }
    return 0;
}

void
_PyMem_QsbrPoll(PyThreadState *tstate)
{
    struct qsbr *qsbr = ((PyThreadStateImpl *)tstate)->qsbr;

    // Process any work on the thread-local queue.
    _PyMem_ProcessQueue(&tstate->mem_work, qsbr, true);

    // Process any work on the interpreter queue if we can get the lock.
    PyInterpreterState *interp = tstate->interp;
    if (_Py_atomic_load_int_relaxed(&interp->mem.nonempty) &&
            _PyMutex_TryLock(&interp->mem.mutex)) {
        int more = _PyMem_ProcessQueue(&interp->mem.work, qsbr, false);
        _Py_atomic_store_int_relaxed(&interp->mem.nonempty, more);
        _PyMutex_unlock(&interp->mem.mutex);
    }
}

void
_PyMem_QsbrFini(PyInterpreterState *interp)
{
    struct _Py_queue_head *queue = &interp->mem.work;
    while (!_Py_queue_is_empty(queue)) {
        _PyMem_WorkBuf *work = _Py_queue_first(queue, _PyMem_WorkBuf, node);
        while (work->first < work->size) {
            _PyMem_WorkItem *item = &work->items[work->first];
            PyMem_Free(item->ptr);
            work->first++;
        }
        _Py_queue_dequeue(queue);
        PyMem_RawFree(work);
    }
    interp->mem.nonempty = 0;
}

void
_PyMem_AbandonQsbr(PyThreadState *tstate)
{
    PyInterpreterState *interp = tstate->interp;

    while (!_Py_queue_is_empty(&tstate->mem_work)) {
        struct _Py_queue_node *node = _Py_queue_dequeue(&tstate->mem_work);
        if (node == NULL) {
            break;
        }
        _PyMem_WorkBuf *work = _Py_queue_data(node, _PyMem_WorkBuf, node);
        if (work->first == work->size) {
            PyMem_RawFree(work);
        }
        else {
            _PyMutex_lock(&interp->mem.mutex);
            _Py_queue_enqeue(&interp->mem.work, node);
            _Py_atomic_store_int_relaxed(&interp->mem.nonempty, 1);
            _PyMutex_unlock(&interp->mem.mutex);
        }
    }
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

void *
PyObject_Malloc(size_t size)
{
    /* see PyMem_RawMalloc() */
    if (size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    OBJECT_STAT_INC_COND(allocations512, size < 512);
    OBJECT_STAT_INC_COND(allocations4k, size >= 512 && size < 4094);
    OBJECT_STAT_INC_COND(allocations_big, size >= 4094);
    OBJECT_STAT_INC(allocations);
    return _PyObject.malloc(_PyObject.ctx, size);
}

void *
PyObject_Calloc(size_t nelem, size_t elsize)
{
    /* see PyMem_RawMalloc() */
    if (elsize != 0 && nelem > (size_t)PY_SSIZE_T_MAX / elsize)
        return NULL;
    OBJECT_STAT_INC_COND(allocations512, elsize < 512);
    OBJECT_STAT_INC_COND(allocations4k, elsize >= 512 && elsize < 4094);
    OBJECT_STAT_INC_COND(allocations_big, elsize >= 4094);
    OBJECT_STAT_INC(allocations);
    return _PyObject.calloc(_PyObject.ctx, nelem, elsize);
}

void *
PyObject_Realloc(void *ptr, size_t new_size)
{
    /* see PyMem_RawMalloc() */
    if (new_size > (size_t)PY_SSIZE_T_MAX)
        return NULL;
    return _PyObject.realloc(_PyObject.ctx, ptr, new_size);
}

void
PyObject_Free(void *ptr)
{
    OBJECT_STAT_INC(frees);
    _PyObject.free(_PyObject.ctx, ptr);
}


/* If we're using GCC, use __builtin_expect() to reduce overhead of
   the valgrind checks */
#if defined(__GNUC__) && (__GNUC__ > 2) && defined(__OPTIMIZE__)
#  define UNLIKELY(value) __builtin_expect((value), 0)
#  define LIKELY(value) __builtin_expect((value), 1)
#else
#  define UNLIKELY(value) (value)
#  define LIKELY(value) (value)
#endif

#ifdef WITH_VALGRIND
#include <valgrind/valgrind.h>

/* -1 indicates that we haven't checked that we're running on valgrind yet. */
static int running_on_valgrind = -1;
#endif

static bool count_blocks(
    const mi_heap_t* heap, const mi_heap_area_t* area,
    void* block, size_t block_size, void* allocated_blocks)
{
    *(size_t *)allocated_blocks += area->used;
    return 1;
}

Py_ssize_t
_Py_GetAllocatedBlocks(void)
{
    // TODO(sgross): this only counts the current thread's blocks.
    size_t allocated_blocks = 0;

    mi_heap_tag_t tags[] = {
        mi_heap_tag_default,
        mi_heap_tag_obj,
        mi_heap_tag_gc,
        mi_heap_tag_list_array,
        mi_heap_tag_dict_keys,
    };
    for (size_t i = 0; i != sizeof(tags)/sizeof(*tags); i++) {
        mi_heap_t *heap = mi_heap_get_tag(tags[i]);
        mi_heap_visit_blocks(heap, false, &count_blocks, &allocated_blocks);
    }

    return (Py_ssize_t)allocated_blocks;
}

/*==========================================================================*/

void *
_PyMem_Malloc(void *ctx, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_malloc(tstate->heaps[mi_heap_tag_default], nbytes);
}

void *
_PyMem_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_calloc(tstate->heaps[mi_heap_tag_default], nelem, elsize);
}

void *
_PyMem_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_realloc(tstate->heaps[mi_heap_tag_default], ptr, nbytes);
}

void
_PyMem_Free(void *ctx, void *p)
{
    mi_free(p);
}

void *
_PyObject_Malloc(void *ctx, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_malloc(tstate->heaps[mi_heap_tag_obj], nbytes);
}

void *
_PyObject_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_calloc(tstate->heaps[mi_heap_tag_obj], nelem, elsize);
}

void *
_PyObject_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_realloc(tstate->heaps[mi_heap_tag_obj], ptr, nbytes);
}

void *
_PyGC_Malloc(void *ctx, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_malloc(tstate->heaps[mi_heap_tag_gc], nbytes);
}

void *
_PyGC_Calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_calloc(tstate->heaps[mi_heap_tag_gc], nelem, elsize);
}

void *
_PyGC_Realloc(void *ctx, void *ptr, size_t nbytes)
{
    PyThreadState *tstate = _PyThreadState_GET();
    return mi_heap_realloc(tstate->heaps[mi_heap_tag_gc], ptr, nbytes);
}

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
       S: nbytes stored as size_t
       I: API identifier (1 byte)
       F: Forbidden bytes (size_t - 1 bytes before, size_t bytes after)
       C: Clean bytes used later to store actual data
       N: Serial number stored as size_t

       If PYMEM_DEBUG_SERIALNO is not defined (default), the last NNNN field
       is omitted. */

    if (use_calloc) {
        p = (uint8_t *)api->alloc.calloc(api->alloc.ctx, 1, total);
    }
    else {
        p = (uint8_t *)api->alloc.malloc(api->alloc.ctx, total);
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

void *
_PyMem_DebugRawMalloc(void *ctx, size_t nbytes)
{
    return _PyMem_DebugRawAlloc(0, ctx, nbytes);
}

void *
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
void
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
    api->alloc.free(api->alloc.ctx, q);
}


void *
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
    r = (uint8_t *)api->alloc.realloc(api->alloc.ctx, head, total);
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

void *
_PyMem_DebugMalloc(void *ctx, size_t nbytes)
{
    _PyMem_DebugCheckGIL(__func__);
    return _PyMem_DebugRawMalloc(ctx, nbytes);
}

void *
_PyMem_DebugCalloc(void *ctx, size_t nelem, size_t elsize)
{
    _PyMem_DebugCheckGIL(__func__);
    return _PyMem_DebugRawCalloc(ctx, nelem, elsize);
}


void
_PyMem_DebugFree(void *ctx, void *ptr)
{
    _PyMem_DebugCheckGIL(__func__);
    _PyMem_DebugRawFree(ctx, ptr);
}


void *
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
    fprintf(stderr, "    %zu bytes originally requested\n", nbytes);

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
    fprintf(stderr,
            "    The block was made by call #%zu to debug malloc/realloc.\n",
            serial);
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
                  "%d %ss * %zd bytes each",
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
