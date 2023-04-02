#ifndef Py_INTERNAL_STACKWALK_H
#define Py_INTERNAL_STACKWALK_H

#include "opcode.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif


struct stack_walk {
    struct _PyThreadStack *ts;
    Register *regs;
    const uint8_t *pc;
    intptr_t offset;
    intptr_t frame_link;
};

static inline void
vm_stack_walk_init(struct stack_walk *w, struct _PyThreadStack *ts)
{
    memset(w, 0, sizeof(*w));
    w->ts = ts;
}

int
vm_stack_walk_lineno(struct stack_walk *w);

static inline int
vm_stack_walk_thread(struct stack_walk *w)
{
    struct _PyThreadStack *ts = w->ts;
    if (w->regs == NULL) {
        w->regs = ts->regs;
        w->pc = ts->pc;
        w->frame_link = 0;
        w->offset = w->regs - ts->stack;
    }
    else {
        // re-fetch regs in case the stack was re-allocated
        // FIXME(sgross): offset depth use ts
        w->regs = &ts->stack[w->offset];

        intptr_t frame_delta = w->regs[-4].as_int64;
        intptr_t frame_link = w->regs[-3].as_int64;
        w->offset -= frame_delta;
        w->pc = (const uint8_t *)(frame_link < 0 ? -frame_link : frame_link);
        w->frame_link = frame_link;
        w->regs -= frame_delta;
    }

    return w->regs > ts->stack;
}

static inline int
vm_stack_walk_all(struct stack_walk *w)
{
    struct _PyThreadStack *ts = w->ts;
    while (ts != NULL) {
        if (vm_stack_walk_thread(w) == 0) {
            // switch to calling virtual thread
            w->ts = ts = ts->prev;
            w->regs = NULL;
            continue;
        }
        return 1;
    }
    return 0;
}

static inline int
vm_stack_walk(struct stack_walk *w)
{
    while (vm_stack_walk_all(w)) {
        PyObject *func = AS_OBJ(w->regs[-1]);
        if (func != NULL && PyFunction_Check(func) && w->pc != NULL) {
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_STACKWALK_H */
