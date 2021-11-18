#ifndef Py_INTERNAL_STACKWALK_H
#define Py_INTERNAL_STACKWALK_H

#include "opcode2.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif


struct stack_walk {
    struct ThreadState *ts;
    Register *regs;
    const uint8_t *pc;
    intptr_t offset;
    intptr_t frame_link;
};

static inline void
vm_stack_walk_init(struct stack_walk *w, struct ThreadState *ts)
{
    memset(w, 0, sizeof(*w));
    w->ts = ts;
}

int
vm_stack_walk_lineno(struct stack_walk *w);

static inline int
vm_stack_walk_thread(struct stack_walk *w)
{
    struct ThreadState *ts = w->ts;
    for (;;) {
        if (w->regs != NULL) {
            intptr_t frame_delta = ts->regs[w->offset-4].as_int64;
            intptr_t frame_link = ts->regs[w->offset-3].as_int64;
            w->offset -= frame_delta;
            w->pc = (const uint8_t *)(frame_link < 0 ? -frame_link : frame_link);
            w->frame_link = frame_link;
        }
        else {
            w->pc = ts->pc;
            w->frame_link = 0;
        }

        if (ts->regs + w->offset == ts->stack) {
            return 0;
        }

        w->regs = &ts->regs[w->offset];
        return 1;
    }
}

static inline int
vm_stack_walk_all(struct stack_walk *w)
{
    struct ThreadState *ts = w->ts;
    while (ts != NULL) {
        if (vm_stack_walk_thread(w) == 0) {
            // switch to calling virtual thread
            w->ts = ts = ts->prev;
            w->offset = 0;
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
        if (PyFunc_Check(AS_OBJ(w->regs[-1])) && w->pc != NULL) {
            return 1;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_STACKWALK_H */
