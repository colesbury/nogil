#ifndef Py_INTERNAL_LIST_H
#define Py_INTERNAL_LIST_H

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// append without acquiring lock
PyAPI_FUNC(int) _PyList_AppendPrivate(PyObject *, PyObject *);

#endif /* !Py_INTERNAL_LIST_H */
