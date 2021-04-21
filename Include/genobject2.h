#ifndef Py_LIMITED_API
#ifndef Py_CEVAL2_GEN_H
#define Py_CEVAL2_GEN_H
#ifdef __cplusplus
extern "C" {
#endif

struct PyGenObject2;
typedef struct PyGenObject2 PyGenObject2;

PyAPI_DATA(PyTypeObject) PyGen2_Type;
PyAPI_DATA(PyTypeObject) PyCoro2_Type;
PyAPI_DATA(PyTypeObject) PyAsyncGen2_Type;
PyAPI_DATA(PyTypeObject) _PyCoroWrapper2_Type;

#define PyGen2_Check(op) PyObject_TypeCheck(op, &PyGen2_Type)
#define PyGen2_CheckExact(op) (Py_TYPE(op) == &PyGen2_Type)
#define PyCoro2_CheckExact(op) (Py_TYPE(op) == &PyCoro2_Type)
#define PyAsyncGen2_CheckExact(op) (Py_TYPE(op) == &PyAsyncGen2_Type)

PyAPI_FUNC(PyObject *) _PyGen2_Send(PyGenObject2 *, PyObject *);

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL2_GEN_H */
#endif /* Py_LIMITED_API */
