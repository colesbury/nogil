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
llist_concat(struct llist_node *head1, struct llist_node *head2)
{
    if (!llist_empty(head2)) {
        head1->prev->next = head2->next;
        head2->next->prev = head1->prev;

        head1->prev = head2->prev;
        head2->prev->next = head1;
        llist_init(head2);
    }
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_LLIST_H */
