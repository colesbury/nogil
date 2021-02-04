#ifndef Py_LIMITED_API
#ifndef Py_CEVAL2_GEN_H
#define Py_CEVAL2_GEN_H
#ifdef __cplusplus
extern "C" {
#endif

enum PyGeneratorStatus {
    GEN_STARTED = 0,
    GEN_YIELD = 1,
    GEN_RUNNING = 2,
    GEN_ERROR = 3,
    GEN_FINISHED = 4
};

/* Generator object interface: move to genobject.h */
typedef struct {
    struct PyVirtualThread base;
    PyObject *weakreflist;
    PyObject *name;
    PyObject *qualname;
    PyObject *return_value;
    char status;
} PyGenObject2;

PyGenObject2 *
PyGen2_NewWithSomething(struct ThreadState *ts);

static inline void
PyGen2_SetNextInstr(PyGenObject2 *gen, const uint32_t *next_instr)
{
    gen->base.thread.next_instr = next_instr;
}

static inline PyGenObject2 *
PyGen2_FromThread(struct ThreadState *ts)
{
    assert(ts->thread_type == THREAD_GENERATOR);
    return (PyGenObject2 *)((char*)ts - offsetof(PyGenObject2, base.thread));
}

extern PyTypeObject PyGen2_Type;

#ifdef __cplusplus
}
#endif
#endif /* !Py_CEVAL2_GEN_H */
#endif /* Py_LIMITED_API */
