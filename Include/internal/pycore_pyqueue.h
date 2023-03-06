#ifndef Py_INTERNAL_PYQUEUE_H
#define Py_INTERNAL_PYQUEUE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

// Implementation of a queue that uses a singly linked list of
// struct _Py_queue_node pointers. The queue is represented by a
// struct _Py_queue_head which contains pointers to the first and
// last node in the queue.

static inline void
_Py_queue_init(struct _Py_queue_head *head)
{
    head->first.next = head->tail = &head->first;
}

static inline bool
_Py_queue_is_empty(struct _Py_queue_head *head)
{
    return head->first.next == &head->first;
}

static inline void
_Py_queue_enqeue(struct _Py_queue_head *head, struct _Py_queue_node *node)
{
    node->next = &head->first;
    head->tail->next = node;
    head->tail = node;
}

static inline struct _Py_queue_node *
_Py_queue_dequeue(struct _Py_queue_head *head)
{
    if (_Py_queue_is_empty(head)) {
        return NULL;
    }
    struct _Py_queue_node *node = head->first.next;
    head->first.next = node->next;
    if (node->next == &head->first) {
        head->tail = &head->first;
    }
    return node;
}

#define _Py_queue_data(node, type, member) \
    (type*)((char*)node - offsetof(type, member))

#define _Py_queue_first(head, type, member) (_Py_queue_data(((head)->first.next), type, member))

#define _Py_queue_last(head, type, member) (_Py_queue_data(((head)->tail), type, member))


#ifdef __cplusplus
}
#endif
#endif  // !Py_INTERNAL_PYQUEUE_H
