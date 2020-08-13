#ifndef Py_INTERNAL_PYREFCNT_H
#define Py_INTERNAL_PYREFCNT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

void _Py_queue_object(PyObject *ob, uintptr_t tid);
void _Py_queue_process(PyThreadState *tstate);
void _Py_queue_create(PyThreadState *tstate);
void _Py_queue_destroy(PyThreadState *tstate);
void _Py_queue_after_fork(PyThreadState *tstate);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYREFCNT_H */
