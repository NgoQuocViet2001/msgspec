#ifndef MSGSPEC_PATTERN_API_H
#define MSGSPEC_PATTERN_API_H

#define MSGSPEC_PATTERN_API_NAME "_msgspec_pcre2._C_API"
#define MSGSPEC_PATTERN_API_VERSION 1

typedef struct {
    unsigned int version;
    size_t size;
    PyTypeObject *matcher_type;
    /* compile may return reference unchanged when automatic matching falls back. */
    PyObject *(*compile)(PyTypeObject *, PyObject *, PyObject *);
    /* match returns 1/0 or -1 on error. */
    int (*match)(PyObject *, PyObject *);
} MsgspecPatternAPI;

#endif
