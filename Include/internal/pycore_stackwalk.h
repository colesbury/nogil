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
    const uint8_t *pc;
    intptr_t offset;
    intptr_t next_offset;
    intptr_t frame_link;
};

static inline bool
frame_link_is_aux(intptr_t frame_link)
{
    return frame_link > 0 && *(const uint8_t *)frame_link == CLEAR_FRAME_AUX;
}

static inline void
vm_stack_walk_init(struct stack_walk *w, struct ThreadState *ts)
{
    memset(w, 0, sizeof(*w));
    w->ts = ts;
    w->frame_link = (intptr_t)ts->pc;
}

static inline Register *
vm_stack_walk_regs(struct stack_walk *w)
{
    return &w->ts->regs[w->offset];
}

static inline int
vm_stack_walk(struct stack_walk *w)
{
    struct ThreadState *ts = w->ts;
    // FIXME(sgross): an if-statement (instead of a loop) should be
    // sufficient, but we currently can have parent threads with empty stacks
    // because of the mix of old and new interpreters.
    while (ts->regs + w->next_offset == ts->stack) {
        if (ts->prev == NULL) {
            return 0;
        }
        // switch to calling virtual thread
        w->ts = ts = ts->prev;
        w->frame_link = (intptr_t)w->ts->pc;
        w->next_offset = 0;
    }

    w->offset = w->next_offset;
    if (frame_link_is_aux(w->frame_link)) {
        w->frame_link = ((struct FrameAux *)w->frame_link)->frame_link;
    }
    w->pc = (const uint8_t *)(w->frame_link < 0 ? -w->frame_link : w->frame_link);

    intptr_t frame_link = ts->regs[w->offset-3].as_int64;
    intptr_t frame_delta = ts->regs[w->offset-4].as_int64;
    w->next_offset = w->offset - frame_delta;
    w->frame_link = frame_link;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_STACKWALK_H */
