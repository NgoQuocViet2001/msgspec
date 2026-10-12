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

#ifndef Py_GIL_DISABLED
#include "pcre2_matcher_api.h"

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
    PyObject *provider;
    PyObject *reference;
    PyObject *error_type;
    int (*match)(PyObject *, PyObject *);
} PatternMatcher;

static int
PatternMatcher_traverse(PatternMatcher *self, visitproc visit, void *arg) {
    Py_VISIT(self->pattern);
    Py_VISIT(self->native);
    Py_VISIT(self->capsule);
    Py_VISIT(self->provider);
    Py_VISIT(self->reference);
    Py_VISIT(self->error_type);
    return 0;
}

static int
PatternMatcher_clear(PatternMatcher *self) {
    self->match = NULL;
    Py_CLEAR(self->pattern);
    Py_CLEAR(self->native);
    Py_CLEAR(self->capsule);
    Py_CLEAR(self->provider);
    Py_CLEAR(self->reference);
    Py_CLEAR(self->error_type);
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

/* A missing optional package is different from a broken installed package. */
static PyObject *
ms_import_optional(const char *module_name, const char *package_name) {
    PyObject *module = PyImport_ImportModule(module_name);
    if (module != NULL || !PyErr_ExceptionMatches(PyExc_ModuleNotFoundError)) return module;
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    PyObject *name = value == NULL ? NULL : PyObject_GetAttrString(value, "name");
    bool absent = name != NULL && PyUnicode_Check(name) &&
                  PyUnicode_CompareWithASCIIString(name, package_name) == 0;
    Py_XDECREF(name);
    PyErr_Clear();
    if (absent) {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }
    else {
        PyErr_Restore(type, value, traceback);
    }
    return NULL;
}

/* Error codes from the external package's PCRE2 10.x interface. Only known
 * accelerator limits permit a return to the already compiled Python regex. */
static bool
ms_external_pattern_fallback(PyObject *error_type, bool compiling) {
    PyObject *type, *value, *traceback;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    bool fallback = false;
    if (type != NULL && PyErr_GivenExceptionMatches(type, error_type) && value != NULL) {
        PyObject *code = PyObject_GetAttrString(value, "code");
        if (code != NULL && PyLong_CheckExact(code)) {
            long rc = PyLong_AsLong(code);
            if (!PyErr_Occurred()) {
                if (compiling) {
                    switch (rc) {
                        case 120: case 132: case 145: case 174: case 175:
                        case 186: case 188: case 201: case -45: case -68:
                            fallback = true;
                    }
                }
                else {
                    fallback = rc == -46 || rc == -47 || rc == -53 || rc == -63;
                }
            }
        }
        Py_XDECREF(code);
    }
    PyErr_Clear();
    if (fallback) {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
    }
    else {
        PyErr_Restore(type, value, traceback);
    }
    return fallback;
}

/* NULL without an exception means this package has no compatible fast API. */
static PyObject *
ms_external_pattern_compile(PyObject *pattern, PyObject *reference) {
    PyObject *provider = ms_import_optional("pcre2._cy", "pcre2");
    if (provider == NULL) return NULL;
    PyObject *capsule = PyObject_GetAttrString(provider, "_MATCHER_API_v1");
    if (capsule == NULL) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError) && PyModule_CheckExact(provider) &&
            PyDict_GetItemString(PyModule_GetDict(provider), "__getattr__") == NULL)
            PyErr_Clear();
        Py_DECREF(provider);
        return NULL;
    }
    PyObject *package = NULL, *compiled = NULL, *native = NULL, *error_type = NULL, *out = NULL;
    PyObject *native_pattern = NULL;
    Pcre2MatcherAPI *api = PyCapsule_GetPointer(capsule, PCRE2_MATCHER_API_NAME);
    if (api == NULL) goto done;
    if (api->abi_version != PCRE2_MATCHER_API_VERSION || api->struct_size != sizeof(Pcre2MatcherAPI) ||
        api->matcher_type == NULL || !PyType_Check(api->matcher_type) ||
        api->new_matcher == NULL || api->search == NULL) {
        PyErr_SetString(PyExc_ImportError, "Incompatible pcre2 matcher API");
        goto done;
    }
    error_type = PyObject_GetAttrString(provider, "LibraryError");
    if (error_type == NULL) goto done;
    if (!PyExceptionClass_Check(error_type)) {
        PyErr_SetString(PyExc_ImportError, "Invalid pcre2 LibraryError type");
        goto done;
    }
    package = PyImport_ImportModule("pcre2");
    if (package == NULL) goto done;
    /* Preserve Python's final-LF $ semantics across PCRE2 build defaults. */
    native_pattern = PyUnicode_FromFormat("(*LF)%U", pattern);
    if (native_pattern == NULL) goto done;
    compiled = PyObject_CallMethod(package, "compile", "O", native_pattern);
    if (compiled == NULL) {
        if (ms_external_pattern_fallback(error_type, true)) out = Py_NewRef(reference);
        goto done;
    }
    native = api->new_matcher(api->matcher_type, compiled, provider);
    if (native == NULL) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "pcre2 matcher creation failed without an exception");
        goto done;
    }
    if (PyErr_Occurred()) goto done;
    if (Py_TYPE(native) != api->matcher_type) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid pcre2 matcher type");
        goto done;
    }
    PatternMatcher *matcher = (PatternMatcher *)PatternMatcher_Type.tp_alloc(&PatternMatcher_Type, 0);
    if (matcher == NULL) goto done;
    matcher->pattern = Py_NewRef(pattern);
    matcher->native = Py_NewRef(native);
    matcher->capsule = Py_NewRef(capsule);
    matcher->provider = Py_NewRef(provider);
    matcher->reference = Py_NewRef(reference);
    matcher->error_type = Py_NewRef(error_type);
    matcher->match = api->search;
    out = (PyObject *)matcher;
done:
    Py_XDECREF(native_pattern);
    Py_XDECREF(native);
    Py_XDECREF(compiled);
    Py_XDECREF(package);
    Py_XDECREF(error_type);
    Py_DECREF(capsule);
    Py_DECREF(provider);
    return out;
}

static PyObject *
ms_pattern_compile(PyObject *pattern, PyObject *reference) {
    if (!PyUnicode_CheckExact(pattern) || !ms_pattern_eligible(pattern))
        return Py_NewRef(reference);
    PyObject *out = ms_external_pattern_compile(pattern, reference);
    if (out != NULL || PyErr_Occurred()) return out;
    return Py_NewRef(reference);
}

static int
ms_pattern_match(PyObject *regex, PyObject *obj) {
    if (Py_TYPE(regex) == &PatternMatcher_Type) {
        PatternMatcher *self = (PatternMatcher *)regex;
        if (!PyUnicode_CheckExact(obj) || !PyUnicode_IS_ASCII(obj))
            return ms_python_pattern_match(self->reference, obj);
        int out = self->match(self->native, obj);
        if (out < 0) {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_RuntimeError, "pcre2 search failed without an exception");
            if (ms_external_pattern_fallback(self->error_type, false))
                return ms_python_pattern_match(self->reference, obj);
            return -1;
        }
        if (PyErr_Occurred()) return -1;
        if (out > 1) {
            PyErr_SetString(PyExc_RuntimeError, "Invalid pcre2 search result");
            return -1;
        }
        return out;
    }
    return ms_python_pattern_match(regex, obj);
}

static int
ms_pattern_init(void) {
    return PyType_Ready(&PatternMatcher_Type);
}
#else
static PyObject *
ms_pattern_compile(PyObject *pattern, PyObject *reference) {
    return Py_NewRef(reference);
}

static int
ms_pattern_match(PyObject *regex, PyObject *obj) {
    return ms_python_pattern_match(regex, obj);
}

static int
ms_pattern_init(void) {
    return 0;
}
#endif
#endif
