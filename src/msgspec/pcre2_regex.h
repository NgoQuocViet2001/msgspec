#ifndef MSGSPEC_PCRE2_REGEX_H
#define MSGSPEC_PCRE2_REGEX_H

static int
ms_python_pattern_match(PyObject *regex, PyObject *obj) {
    PyObject *res = PyObject_CallMethod(regex, "search", "O", obj);
    if (res == NULL) return -1;
    int out = res != Py_None;
    Py_DECREF(res);
    return out;
}

#include "pattern_api.h"

/* Only this positive grammar is eligible for automatic native matching.
 * Python compilation still occurs first, including its errors and warnings. */
static bool
ms_pattern_char(const char *s, Py_ssize_t end, Py_ssize_t *pos, int *value) {
    if (*pos >= end) return false;
    unsigned char c = s[(*pos)++];
    if (c == '\\') {
        if (*pos >= end) return false;
        c = s[(*pos)++];
        if (c < 32 || c > 126 || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return false;
    }
    else if (c < 32 || c > 126) {
        return false;
    }
    *value = c;
    return true;
}

static bool
ms_pattern_bound(const char *s, Py_ssize_t end, Py_ssize_t *pos, unsigned *value) {
    unsigned out = 0;
    Py_ssize_t start = *pos;
    while (*pos < end && s[*pos] >= '0' && s[*pos] <= '9') {
        out = out * 10 + (unsigned)(s[(*pos)++] - '0');
        if (out >= 65536) return false;
    }
    *value = out;
    return *pos != start;
}

static bool
ms_pattern_eligible(PyObject *pattern) {
    if (!PyUnicode_IS_ASCII(pattern)) return false;
    const char *s = (const char *)PyUnicode_1BYTE_DATA(pattern);
    Py_ssize_t n = PyUnicode_GET_LENGTH(pattern), pos = 1, end = n - 1;
    if (n < 2 || s[0] != '^' || s[end] != '$') return false;
    while (pos < end) {
        char c = s[pos];
        if (c == '[') {
            pos++;
            if (pos >= end || s[pos] == '^' || s[pos] == ']') return false;
            bool first = true;
            while (pos < end && s[pos] != ']') {
                if (s[pos] == '[') return false;
                if (pos + 1 < end && s[pos] == s[pos + 1] &&
                    (s[pos] == '&' || s[pos] == '-' || s[pos] == '~' || s[pos] == '|')) return false;
                int left;
                if (!ms_pattern_char(s, end, &pos, &left)) return false;
                if (left == '-' && !first && s[pos] != ']') return false;
                if (pos + 1 < end && s[pos] == '-' && s[pos + 1] != ']') {
                    int right;
                    pos++;
                    if (s[pos] == '[' || s[pos] == ']') return false;
                    if (!ms_pattern_char(s, end, &pos, &right) || left > right) return false;
                }
                first = false;
            }
            if (pos >= end || first) return false;
            pos++;
        }
        else {
            if (c != '\\' && strchr(".^$*+?{}()[]|", c) != NULL) return false;
            int value;
            if (!ms_pattern_char(s, end, &pos, &value)) return false;
        }
        if (pos < end && (s[pos] == '?' || s[pos] == '*' || s[pos] == '+')) {
            pos++;
            if (pos < end && s[pos] == '?') pos++;
        }
        else if (pos < end && s[pos] == '{') {
            unsigned lower, upper;
            pos++;
            if (!ms_pattern_bound(s, end, &pos, &lower)) return false;
            if (pos < end && s[pos] == ',') {
                pos++;
                if (pos < end && s[pos] != '}') {
                    if (!ms_pattern_bound(s, end, &pos, &upper) || upper < lower) return false;
                }
            }
            if (pos >= end || s[pos++] != '}') return false;
            if (pos < end && s[pos] == '?') pos++;
        }
    }
    return true;
}

typedef struct {
    PyObject_HEAD
    PyObject *pattern;
    PyObject *native;
    PyObject *capsule;
    MsgspecPatternAPI *api;
} PatternMatcher;

static int
PatternMatcher_traverse(PatternMatcher *self, visitproc visit, void *arg) {
    Py_VISIT(self->pattern);
    Py_VISIT(self->native);
    Py_VISIT(self->capsule);
    return 0;
}

static int
PatternMatcher_clear(PatternMatcher *self) {
    self->api = NULL;
    Py_CLEAR(self->pattern);
    Py_CLEAR(self->native);
    Py_CLEAR(self->capsule);
    return 0;
}

static void
PatternMatcher_dealloc(PatternMatcher *self) {
    PyObject_GC_UnTrack(self);
    PatternMatcher_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef PatternMatcher_members[] = {
    {"pattern", T_OBJECT, offsetof(PatternMatcher, pattern), READONLY, NULL},
    {NULL},
};

static PyTypeObject PatternMatcher_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "msgspec._PatternMatcher",
    .tp_basicsize = sizeof(PatternMatcher),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)PatternMatcher_traverse,
    .tp_clear = (inquiry)PatternMatcher_clear,
    .tp_dealloc = (destructor)PatternMatcher_dealloc,
    .tp_members = PatternMatcher_members,
};

static PyObject *
ms_pattern_compile(PyObject *pattern, PyObject *reference) {
    if (!ms_pattern_eligible(pattern))
        return Py_NewRef(reference);
    PyObject *module = PyImport_ImportModule("_msgspec_pcre2");
    if (module == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_ModuleNotFoundError)) return NULL;
        PyObject *type, *value, *traceback;
        PyErr_Fetch(&type, &value, &traceback);
        PyErr_NormalizeException(&type, &value, &traceback);
        PyObject *name = value == NULL ? NULL : PyObject_GetAttrString(value, "name");
        bool absent = name != NULL && PyUnicode_Check(name) &&
                      PyUnicode_CompareWithASCIIString(name, "_msgspec_pcre2") == 0;
        Py_XDECREF(name);
        if (!absent) {
            PyErr_Clear();
            PyErr_Restore(type, value, traceback);
            return NULL;
        }
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return Py_NewRef(reference);
    }
    PyObject *capsule = PyObject_GetAttrString(module, "_C_API");
    Py_DECREF(module);
    if (capsule == NULL) return NULL;
    MsgspecPatternAPI *api = PyCapsule_GetPointer(capsule, MSGSPEC_PATTERN_API_NAME);
    if (api == NULL) {
        Py_DECREF(capsule);
        return NULL;
    }
    if (api->version != MSGSPEC_PATTERN_API_VERSION || api->size != sizeof(MsgspecPatternAPI) ||
        api->matcher_type == NULL || !PyType_Check(api->matcher_type) ||
        api->compile == NULL || api->match == NULL) {
        Py_DECREF(capsule);
        PyErr_SetString(PyExc_ImportError, "Incompatible msgspec-pcre2 backend");
        return NULL;
    }
    PyObject *native = api->compile(api->matcher_type, pattern, reference);
    if (native == NULL || native == reference) {
        Py_DECREF(capsule);
        return native;
    }
    if (Py_TYPE(native) != api->matcher_type) {
        Py_DECREF(native);
        Py_DECREF(capsule);
        PyErr_SetString(PyExc_RuntimeError, "Invalid msgspec-pcre2 matcher");
        return NULL;
    }
    PatternMatcher *out = (PatternMatcher *)PatternMatcher_Type.tp_alloc(&PatternMatcher_Type, 0);
    if (out == NULL) {
        Py_DECREF(native);
        Py_DECREF(capsule);
        return NULL;
    }
    out->pattern = Py_NewRef(pattern);
    out->native = native;
    out->capsule = capsule;
    out->api = api;
    return (PyObject *)out;
}

static int
ms_pattern_match(PyObject *regex, PyObject *obj) {
    if (Py_TYPE(regex) == &PatternMatcher_Type) {
        PatternMatcher *self = (PatternMatcher *)regex;
        return self->api->match(self->native, obj);
    }
    return ms_python_pattern_match(regex, obj);
}

static int
ms_pattern_init(void) {
    return PyType_Ready(&PatternMatcher_Type);
}
#endif
