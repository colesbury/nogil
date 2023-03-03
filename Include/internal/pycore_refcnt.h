#ifndef Py_INTERNAL_PYREFCNT_H
#define Py_INTERNAL_PYREFCNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#define PYOBJECT_QUEUE_SIZE 254

typedef struct _PyObjectQueue {
    struct _PyObjectQueue *prev;
    Py_ssize_t n;
    PyObject *objs[PYOBJECT_QUEUE_SIZE];
} _PyObjectQueue;

#define _PyObjectQueue_ForEach(q, obj) \
    for (obj = _PyObjectQueue_Pop(q); obj != NULL; obj = _PyObjectQueue_Pop(q))

_PyObjectQueue *_PyObjectQueue_New(void);

static inline void
_PyObjectQueue_Push(_PyObjectQueue **queue_ptr, PyObject *obj)
{
    _PyObjectQueue *q = *queue_ptr;
    if (q == NULL || q->n == PYOBJECT_QUEUE_SIZE) {
        _PyObjectQueue *tmp = _PyObjectQueue_New();
        tmp->prev = q;
        *queue_ptr = q = tmp;
    }
    q->objs[q->n] = obj;
    q->n++;
}

static inline PyObject *
_PyObjectQueue_Pop(_PyObjectQueue **queue_ptr)
{
    _PyObjectQueue *q = *queue_ptr;
    if (q == NULL) {
        return NULL;
    }
    while (q->n == 0) {
        _PyObjectQueue *prev = q->prev;
        PyMem_RawFree(q);
        *queue_ptr = q = prev;
        if (q == NULL) {
            return NULL;
        }
    }
    q->n--;
    return q->objs[q->n];
}

// Enqueues an object to be merged by it's owning thread (tid). This
// steals a reference to the object.
void _Py_queue_object(PyObject *ob, uintptr_t tid);

void _Py_queue_process(PyThreadState *tstate);
void _Py_queue_process_gc(PyThreadState *tstate, _PyObjectQueue **queue_ptr);
void _Py_queue_create(PyThreadState *tstate);
void _Py_queue_destroy(PyThreadState *tstate);
void _Py_queue_after_fork(void);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYREFCNT_H */
