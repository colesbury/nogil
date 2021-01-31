#ifndef Py_INTERNAL_CODE_H
#define Py_INTERNAL_CODE_H
#ifdef __cplusplus
extern "C" {
#endif
 
typedef struct {
    PyObject *ptr;  /* Cached pointer (borrowed reference) */
    uint64_t globals_ver;  /* ma_version of global dict */
    uint64_t builtins_ver; /* ma_version of builtin dict */
} _PyOpcache_LoadGlobal;

struct _PyOpcache {
    union {
        _PyOpcache_LoadGlobal lg;
    } u;
    char optimized;
};

typedef struct {
    Py_ssize_t start;   /* start instr for try block range */
    Py_ssize_t handler; /* end instr try block AND start of handler range */
    Py_ssize_t handler_end; /* end of handler block */
    Py_ssize_t reg;     /* temporary register to store active exception */
} ExceptionHandler;

struct _PyHandlerTable {
    Py_ssize_t size;
    ExceptionHandler entries[];
};

/* Private API */
int _PyCode_InitOpcache(PyCodeObject *co);

PyCodeObject *
PyCode_NewInternal(
        int, int, int, int, int, int, int, int, PyObject *, PyObject *,
        PyObject *, PyObject *, PyObject *, PyObject *,
        PyObject *, PyObject *, int, PyObject *);


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CODE_H */
