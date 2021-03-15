
/* Function object interface */
#ifndef Py_LIMITED_API
#ifndef Py_FUNCOBJECT2_H
#define Py_FUNCOBJECT2_H
#ifdef __cplusplus
extern "C" {
#endif

/* Function objects and code objects should not be confused with each other:
 *
 * Function objects are created by the execution of the 'def' statement.
 * They reference a code object in their __code__ attribute, which is a
 * purely syntactic object, i.e. nothing more than a compiled version of some
 * source code lines.  There is one code object per source code "fragment",
 * but each code object can be referenced by zero or many function objects
 * depending only on how many times the 'def' statement in the source was
 * executed so far.
 */

typedef struct {
    PyFuncBase func_base;
    PyObject *globals;
    PyObject *builtins;
    PyObject *func_doc;         /* The __doc__ attribute, can be anything */
    PyObject *func_name;        /* The __name__ attribute, a string object */
    PyObject *func_dict;        /* The __dict__ attribute, a dict or NULL */
    PyObject *func_weakreflist; /* List of weak references */
    PyObject *func_module;      /* The __module__ attribute, can be anything */
    PyObject *func_annotations; /* Annotations, a dict or NULL */
    PyObject *func_qualname;    /* The qualified name */
    vectorcallfunc vectorcall;
    PyObject *freevars[0];  // captured variables and default argument values
    // closure... LuaJit has closed-over variables as flexiable array member
} PyFunc;

PyAPI_DATA(PyTypeObject) PyFunc_Type;

#define PyFunc_Check(op) (Py_TYPE(op) == &PyFunc_Type)

PyAPI_FUNC(PyObject *) PyFunc_New(PyObject *, PyObject *);

#ifdef __cplusplus
}
#endif
#endif /* !Py_FUNCOBJECT2_H */
#endif /* Py_LIMITED_API */
