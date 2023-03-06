#ifndef Py_PYQUEUE_H
#define Py_PYQUEUE_H
/* Header excluded from the stable API */
#ifndef Py_LIMITED_API

// See pycore_pyqueue.h for the implementation of the queue API.

struct _Py_queue_node {
    struct _Py_queue_node *next;
};

struct _Py_queue_head {
    struct _Py_queue_node first;
    struct _Py_queue_node *tail;
};

#endif /* !defined(Py_LIMITED_API) */
#endif /* !Py_PYQUEUE_H */
