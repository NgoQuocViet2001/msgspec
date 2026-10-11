#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <pcre2.h>
#include "pattern_api.h"

static int
ms_python_pattern_match(PyObject *regex, PyObject *obj) {
    PyObject *res = PyObject_CallMethod(regex, "search", "O", obj);
    if (res == NULL) return -1;
    int out = res != Py_None;
    Py_DECREF(res);
    return out;
}

/* PCRE2 initializes a process-static, non-atomic executable allocator flag.
 * Warm it once before independent concurrent JIT compilations. This guard
 * covers only the native allocator probe, never matching or Python calls. */
static atomic_flag ms_pcre2_jit_init_lock = ATOMIC_FLAG_INIT;
static atomic_bool ms_pcre2_jit_initialized = ATOMIC_VAR_INIT(false);
static int ms_pcre2_jit_init_status;

static int
ms_pcre2_jit_initialize(void) {
    if (atomic_load_explicit(&ms_pcre2_jit_initialized, memory_order_acquire))
        return ms_pcre2_jit_init_status;
    while (atomic_flag_test_and_set_explicit(&ms_pcre2_jit_init_lock, memory_order_acquire)) {}
    if (!atomic_load_explicit(&ms_pcre2_jit_initialized, memory_order_relaxed)) {
        ms_pcre2_jit_init_status = pcre2_jit_compile(NULL, PCRE2_JIT_TEST_ALLOC);
        atomic_store_explicit(&ms_pcre2_jit_initialized, true, memory_order_release);
    }
    atomic_flag_clear_explicit(&ms_pcre2_jit_init_lock, memory_order_release);
    return ms_pcre2_jit_init_status;
}

typedef struct {
    PyObject_HEAD
    PyObject *pattern;
    PyObject *reference;
    pcre2_code *code;
    pcre2_match_data *scratch;
    PyThread_type_lock lock;
    bool jit;
} PatternMatcher;

static int
PatternMatcher_traverse(PatternMatcher *self, visitproc visit, void *arg) {
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->pattern);
    Py_VISIT(self->reference);
    return 0;
}

static int
PatternMatcher_clear(PatternMatcher *self) {
    Py_CLEAR(self->pattern);
    Py_CLEAR(self->reference);
    return 0;
}

static void
PatternMatcher_dealloc(PatternMatcher *self) {
    PyObject_GC_UnTrack(self);
    PatternMatcher_clear(self);
    if (self->scratch != NULL) pcre2_match_data_free(self->scratch);
    if (self->code != NULL) pcre2_code_free(self->code);
    if (self->lock != NULL) PyThread_free_lock(self->lock);
    PyTypeObject *type = Py_TYPE(self);
    type->tp_free((PyObject *)self);
    Py_DECREF(type);
}

static PyMemberDef PatternMatcher_members[] = {
    {"pattern", T_OBJECT, offsetof(PatternMatcher, pattern), READONLY, NULL},
    {NULL},
};

static PyObject *
PatternMatcher_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    PyErr_SetString(PyExc_TypeError, "Pattern objects cannot be created directly");
    return NULL;
}

static PyType_Slot PatternMatcher_slots[] = {
    {Py_tp_traverse, PatternMatcher_traverse},
    {Py_tp_clear, PatternMatcher_clear},
    {Py_tp_dealloc, PatternMatcher_dealloc},
    {Py_tp_members, PatternMatcher_members},
    {Py_tp_new, PatternMatcher_new},
    {0, NULL},
};

static PyType_Spec PatternMatcher_spec = {
    .name = "_msgspec_pcre2.Pattern",
    .basicsize = sizeof(PatternMatcher),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = PatternMatcher_slots,
};

static void
ms_pcre2_error(int code, const char *operation) {
    if (code == PCRE2_ERROR_NOMEMORY || code == PCRE2_ERROR_HEAP_FAILED) {
        PyErr_NoMemory();
        return;
    }
    PCRE2_UCHAR message[256];
    int n = pcre2_get_error_message(code, message, sizeof(message));
    if (n < 0) PyErr_Format(PyExc_RuntimeError, "PCRE2 %s failed (%d)", operation, code);
    else PyErr_Format(PyExc_RuntimeError, "PCRE2 %s failed: %s (%d)", operation, message, code);
}
static PyObject *
ms_pcre2_compile(PyTypeObject *type, PyObject *pattern, PyObject *reference) {
    Py_ssize_t size;
    const char *raw = PyUnicode_AsUTF8AndSize(pattern, &size);
    if (raw == NULL) return NULL;
    pcre2_compile_context *context = pcre2_compile_context_create(NULL);
    if (context == NULL) return PyErr_NoMemory();
    int status = pcre2_set_newline(context, PCRE2_NEWLINE_LF);
    if (status != 0) {
        pcre2_compile_context_free(context);
        ms_pcre2_error(status, "configuration");
        return NULL;
    }
    int error;
    PCRE2_SIZE offset;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)raw, (PCRE2_SIZE)size,
                                    PCRE2_UTF | PCRE2_UCP, &error, &offset, context);
    pcre2_compile_context_free(context);
    if (code == NULL) {
        if (error == PCRE2_ERROR_HEAP_FAILED) return PyErr_NoMemory();
        if (error == PCRE2_ERROR_PATTERN_TOO_LARGE || error == PCRE2_ERROR_PATTERN_TOO_COMPLICATED ||
             error == PCRE2_ERROR_PATTERN_STRING_TOO_LONG ||
             error == PCRE2_ERROR_PATTERN_COMPILED_SIZE_TOO_BIG ||
             error == PCRE2_ERROR_UNICODE_NOT_SUPPORTED ||
             error == PCRE2_ERROR_UNICODE_PROPERTIES_UNAVAILABLE ||
             error == PCRE2_ERROR_UTF_IS_DISABLED || error == PCRE2_ERROR_UCP_IS_DISABLED)
            return Py_NewRef(reference);
        PCRE2_UCHAR message[256];
        int n = pcre2_get_error_message(error, message, sizeof(message));
        if (n < 0) PyErr_Format(PyExc_ValueError, "Invalid PCRE2 pattern at offset %zu (%d)", (size_t)offset, error);
        else PyErr_Format(PyExc_ValueError, "Invalid PCRE2 pattern at offset %zu: %s", (size_t)offset, message);
        return NULL;
    }
    PatternMatcher *out = (PatternMatcher *)type->tp_alloc(type, 0);
    if (out == NULL) {
        pcre2_code_free(code);
        return NULL;
    }
    out->code = code;
    out->pattern = Py_NewRef(pattern);
    out->reference = Py_NewRef(reference);
    out->scratch = pcre2_match_data_create_from_pattern(code, NULL);
    out->lock = PyThread_allocate_lock();
    if (out->scratch == NULL || out->lock == NULL) {
        PyErr_NoMemory();
        goto error;
    }
    uint32_t available = 0;
    status = pcre2_config(PCRE2_CONFIG_JIT, &available);
    if (status != 0) {
        ms_pcre2_error(status, "configuration");
        goto error;
    }
    if (available) {
        status = ms_pcre2_jit_initialize();
        if (status == 0) status = pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);
        if (status == 0) out->jit = true;
        /* JIT is optional. NOMEMORY also denotes unavailable executable memory
         * in PCRE2's allocator probe, so keep the valid interpreter code.
         * The probe's process-wide failure remains cached, as in PCRE2 itself. */
        else if (status != PCRE2_ERROR_JIT_BADOPTION && status != PCRE2_ERROR_JIT_UNSUPPORTED &&
                 status != PCRE2_ERROR_NOMEMORY) {
            ms_pcre2_error(status, "JIT compilation");
            goto error;
        }
    }
    return (PyObject *)out;
error:
    Py_DECREF(out);
    return NULL;
}

static int
ms_pcre2_match(PyObject *regex, PyObject *obj) {
    PatternMatcher *self = (PatternMatcher *)regex;
    if (!PyUnicode_IS_ASCII(obj))
        return ms_python_pattern_match(self->reference, obj);
    Py_ssize_t size;
    const char *raw = PyUnicode_AsUTF8AndSize(obj, &size);
    if (raw == NULL) return -1;
    bool acquired = PyThread_acquire_lock(self->lock, 0);
    pcre2_match_data *scratch = acquired ? self->scratch : pcre2_match_data_create_from_pattern(self->code, NULL);
    if (scratch == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    int rc = pcre2_match(self->code, (PCRE2_SPTR)raw, (PCRE2_SIZE)size, 0,
                         self->jit ? 0 : PCRE2_NO_JIT, scratch, NULL);
    if (acquired) PyThread_release_lock(self->lock);
    else pcre2_match_data_free(scratch);
    if (rc >= 0) return 1;
    if (rc == PCRE2_ERROR_NOMATCH) return 0;
    if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT ||
         rc == PCRE2_ERROR_HEAPLIMIT || rc == PCRE2_ERROR_JIT_STACKLIMIT)
        return ms_python_pattern_match(self->reference, obj);
    ms_pcre2_error(rc, "matching");
    return -1;
}

static void
ms_pcre2_api_free(PyObject *capsule) {
    MsgspecPatternAPI *api = PyCapsule_GetPointer(capsule, MSGSPEC_PATTERN_API_NAME);
    if (api != NULL) {
        Py_DECREF(api->matcher_type);
        PyMem_Free(api);
    }
}

static int
ms_pcre2_exec(PyObject *module) {
    PyObject *type = PyType_FromSpec(&PatternMatcher_spec);
    if (type == NULL) return -1;
    MsgspecPatternAPI *api = PyMem_Malloc(sizeof(MsgspecPatternAPI));
    if (api == NULL) {
        Py_DECREF(type);
        PyErr_NoMemory();
        return -1;
    }
    *api = (MsgspecPatternAPI){
        .version = MSGSPEC_PATTERN_API_VERSION,
        .size = sizeof(MsgspecPatternAPI),
        .matcher_type = (PyTypeObject *)type,
        .compile = ms_pcre2_compile,
        .match = ms_pcre2_match,
    };
    PyObject *capsule = PyCapsule_New(api, MSGSPEC_PATTERN_API_NAME, ms_pcre2_api_free);
    if (capsule == NULL) {
        Py_DECREF(type);
        PyMem_Free(api);
        return -1;
    }
    if (PyModule_AddObject(module, "_C_API", capsule) < 0) {
        Py_DECREF(capsule);
        return -1;
    }
    return 0;
}

static PyModuleDef_Slot ms_pcre2_slots[] = {
    {Py_mod_exec, ms_pcre2_exec},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL},
};

static struct PyModuleDef ms_pcre2_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_msgspec_pcre2",
    .m_size = 0,
    .m_slots = ms_pcre2_slots,
};

PyMODINIT_FUNC
PyInit__msgspec_pcre2(void) {
    return PyModuleDef_Init(&ms_pcre2_module);
}
