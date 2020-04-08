#ifndef Py_INTERNAL_LLIST_H
#define Py_INTERNAL_LLIST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "Py_BUILD_CORE must be defined to include this header"
#endif

struct llist_node {
    struct llist_node *next;
    struct llist_node *prev;
};

#define llist_data(node, type, member) \
    (type*)((char*)node - offsetof(type, member))

#define llist_for_each(node, head) \
    for (node = (head)->next; node != (head); node = node->next)

#define LLIST_INIT(head) { &head, &head }

static inline void
llist_init(struct llist_node *head)
{
    head->next = head;
    head->prev = head;
}

static inline int
llist_empty(struct llist_node *head)
{
    return head->next == head;
}

static inline void
llist_insert_tail(struct llist_node *head, struct llist_node *node)
{
    node->prev = head->prev;
    node->next = head;
    head->prev->next = node;
    head->prev = node;
}

static inline void
llist_remove(struct llist_node *node)
{
    struct llist_node *prev = node->prev;
    struct llist_node *next = node->next;
    prev->next = next;
    next->prev = prev;
    node->prev = NULL;
    node->next = NULL;
}

static inline void
llist_move_all(struct llist_node *dst, struct llist_node *src)
{
    if (llist_empty(src)) {
        return;
    }
    struct llist_node *first = src->next;
    struct llist_node *last = src->prev;
    struct llist_node *tail = dst->prev;
    tail->next = first;
    first->prev = tail;
    dst->prev = last;
    last->next = dst;
    llist_init(src);
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_LLIST_H */
