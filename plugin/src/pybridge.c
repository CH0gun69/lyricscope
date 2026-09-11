#include <Python.h>

#include "pybridge.h"
#include "log.h"

#include <deadbeef/deadbeef.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

extern DB_functions_t *deadbeef;

/* The libpython soname is built from the headers this was compiled
 * against, not hardcoded: "libpython3.12.so.1.0" is only right on a
 * machine running 3.12, and getting it wrong means the plugin loads and
 * then silently never resolves a single lyric. */
#define LS_STR2(x) #x
#define LS_STR(x) LS_STR2(x)
#define LS_LIBPYTHON                                                           \
    "libpython" LS_STR(PY_MAJOR_VERSION) "." LS_STR(PY_MINOR_VERSION) ".so.1.0"
#define LS_LIBPYTHON_ALT                                                       \
    "libpython" LS_STR(PY_MAJOR_VERSION) "." LS_STR(PY_MINOR_VERSION) ".so"

/* Where core/ lives. Deliberately not a compile-time path: a build
 * directory is wherever the person who cloned it happened to put the
 * repo, and baking that in is what stops the plugin working anywhere but
 * the machine it was built on.
 *
 * Checked in order:
 *   1. lyricscope.python_home in DeaDBeeF's config — point this at a
 *      checkout to run from source without installing.
 *   2. $XDG_DATA_HOME/lyricscope (default ~/.local/share/lyricscope),
 *      which is where `make install` puts it. */
static char *python_home(void) {
    char configured[4096] = {0};
    deadbeef->conf_get_str("lyricscope.python_home", "", configured,
                           sizeof(configured));
    if (configured[0]) {
        return strdup(configured);
    }

    const char *xdg = getenv("XDG_DATA_HOME");
    char *path = NULL;
    if (xdg && *xdg) {
        if (asprintf(&path, "%s/lyricscope", xdg) < 0) return NULL;
    } else {
        const char *home = getenv("HOME");
        if (asprintf(&path, "%s/.local/share/lyricscope", home ? home : ".") < 0)
            return NULL;
    }
    return path;
}

static void *libpython;
static PyObject *resolve_fn;
static int ready;

/* Report and clear a pending Python exception. Must hold the GIL. */
static void log_py_error(const char *what) {
    if (!PyErr_Occurred()) {
        LS_LOG("%s: failed with no exception set", what);
        return;
    }
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    PyErr_NormalizeException(&type, &value, &tb);
    PyObject *text = value ? PyObject_Str(value) : NULL;
    const char *utf8 = text ? PyUnicode_AsUTF8(text) : NULL;
    LS_LOG("%s: %s: %s", what,
           type ? ((PyTypeObject *)type)->tp_name : "?", utf8 ? utf8 : "?");
    Py_XDECREF(text);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
}

int ls_py_init(void) {
    if (ready) {
        return 0;
    }

    /* RTLD_GLOBAL, explicitly, before anything else touches Python.
     *
     * DeaDBeeF dlopen()s plugins into a local symbol scope and a library
     * pulled in via DT_NEEDED inherits that scope. Python's own C
     * extension modules do not link against libpython themselves — they
     * expect its symbols to be globally visible in the process — so
     * without this an `import` of any of them fails with an undefined
     * symbol even though libpython is plainly loaded. */
    libpython = dlopen(LS_LIBPYTHON, RTLD_NOW | RTLD_GLOBAL);
    if (!libpython) {
        /* The versioned soname ships with the runtime package; the bare
         * one is a symlink from the -dev package. Either will do. */
        libpython = dlopen(LS_LIBPYTHON_ALT, RTLD_NOW | RTLD_GLOBAL);
    }
    if (!libpython) {
        LS_LOG("dlopen %s failed: %s", LS_LIBPYTHON, dlerror());
        return -1;
    }

    PyConfig config;
    /* Isolated: the player's environment cannot redirect what gets
     * imported — no PYTHONPATH, no user site-packages, no sitecustomize.
     * The backend's own directory is added explicitly below. */
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 1;

    /* The real interpreter binary, not a script path: Python derives its
     * stdlib prefix from program_name, so pointing it at something that
     * may not exist makes the whole import machinery depend on a file
     * this plugin does not otherwise need. */
    PyStatus status = PyConfig_SetBytesString(&config, &config.program_name,
                                              "/usr/bin/python3");
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        LS_LOG("PyConfig_SetBytesString failed");
        return -1;
    }

    status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        LS_LOG("Py_InitializeFromConfig failed: %s",
               status.err_msg ? status.err_msg : "?");
        return -1;
    }

    /* Third-party imports (mutagen) come from the system interpreter's own
     * site-packages, which site.py has already put on the path; only the
     * backend's own directory has to be added. */
    char *home = python_home();
    if (!home) {
        LS_LOG("could not work out where the python backend lives");
        PyEval_SaveThread();
        return -1;
    }
    /* Inserted through the C API rather than by running a generated
     * "sys.path.insert(0, '...')" string: a home directory containing a
     * quote or a backslash would otherwise be a code-injection bug in our
     * own startup path. */
    PyObject *sys_path = PySys_GetObject("path"); /* borrowed */
    PyObject *entry = PyUnicode_FromString(home);
    if (!sys_path || !entry || PyList_Insert(sys_path, 0, entry) != 0) {
        LS_LOG("failed to add %s to sys.path", home);
        Py_XDECREF(entry);
        free(home);
        PyEval_SaveThread();
        return -1;
    }
    Py_DECREF(entry);
    LS_LOG("python backend: %s", home);
    free(home);

    PyObject *module = PyImport_ImportModule("core.resolver");
    if (!module) {
        log_py_error("import core.resolver");
        PyEval_SaveThread();
        return -1;
    }
    resolve_fn = PyObject_GetAttrString(module, "resolve");
    Py_DECREF(module);
    if (!resolve_fn) {
        log_py_error("getattr core.resolver.resolve");
        PyEval_SaveThread();
        return -1;
    }

    /* Hand the GIL back: Py_InitializeFromConfig leaves it held by this
     * thread, and keeping it would deadlock the first call that arrives
     * from any other one. */
    PyEval_SaveThread();
    ready = 1;
    LS_LOG("embedded python %.12s ready", Py_GetVersion());
    return 0;
}

void ls_lyrics_clear(LsLyrics *lyrics) {
    if (!lyrics) {
        return;
    }
    for (int i = 0; i < lyrics->count; i++) {
        free(lyrics->lines[i].text);
    }
    free(lyrics->lines);
    free(lyrics->source);
    memset(lyrics, 0, sizeof(*lyrics));
}

/* Copy a Python str attribute into a fresh C string, or NULL. */
static char *dup_str_attr(PyObject *object, const char *name) {
    PyObject *value = PyObject_GetAttrString(object, name);
    if (!value) {
        PyErr_Clear();
        return NULL;
    }
    char *copy = NULL;
    if (PyUnicode_Check(value)) {
        const char *utf8 = PyUnicode_AsUTF8(value);
        copy = utf8 ? strdup(utf8) : NULL;
    }
    Py_DECREF(value);
    return copy;
}

int ls_py_resolve(const char *path, const char *artist, const char *title,
                  LsLyrics *out) {
    ls_lyrics_clear(out);
    if (!ready) {
        return -1;
    }

    /* Ensure/Release rather than assuming a thread state: this is reached
     * from the GTK main thread, which is not the thread that initialised
     * the interpreter. */
    PyGILState_STATE gil = PyGILState_Ensure();

    PyObject *args = Py_BuildValue("(sss)", path ? path : "",
                                   artist ? artist : "", title ? title : "");
    PyObject *resolved = args ? PyObject_CallObject(resolve_fn, args) : NULL;
    Py_XDECREF(args);
    if (!resolved) {
        log_py_error("core.resolver.resolve()");
        PyGILState_Release(gil);
        return -1;
    }

    PyObject *lyrics = PyObject_GetAttrString(resolved, "lyrics");
    if (!lyrics) {
        log_py_error("Resolved.lyrics");
        Py_DECREF(resolved);
        PyGILState_Release(gil);
        return -1;
    }

    PyObject *lines = PyObject_GetAttrString(lyrics, "lines");
    PyObject *fast = lines ? PySequence_Fast(lines, "lines") : NULL;
    if (!fast) {
        log_py_error("Lyrics.lines");
        Py_XDECREF(lines);
        Py_DECREF(lyrics);
        Py_DECREF(resolved);
        PyGILState_Release(gil);
        return -1;
    }

    const Py_ssize_t count = PySequence_Fast_GET_SIZE(fast);
    out->lines = count > 0 ? calloc((size_t)count, sizeof(LsLine)) : NULL;
    if (count > 0 && !out->lines) {
        Py_DECREF(fast);
        Py_DECREF(lines);
        Py_DECREF(lyrics);
        Py_DECREF(resolved);
        PyGILState_Release(gil);
        return -1;
    }

    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *line = PySequence_Fast_GET_ITEM(fast, i); /* borrowed */
        char *text = dup_str_attr(line, "text");
        out->lines[i].text = text ? text : strdup("");

        /* Line.time is Optional[float]: None on an unsynced line. A
         * negative time stands in for that here, so the draw path never
         * has to carry a separate "has a timestamp" flag. */
        PyObject *when = PyObject_GetAttrString(line, "time");
        if (when && PyFloat_Check(when)) {
            out->lines[i].time = PyFloat_AsDouble(when);
        } else if (when && PyLong_Check(when)) {
            out->lines[i].time = (double)PyLong_AsLong(when);
        } else {
            out->lines[i].time = -1.0;
            PyErr_Clear();
        }
        Py_XDECREF(when);
    }
    out->count = (int)count;

    PyObject *synced = PyObject_GetAttrString(lyrics, "synced");
    out->synced = synced ? PyObject_IsTrue(synced) : 0;
    Py_XDECREF(synced);

    out->source = dup_str_attr(resolved, "source");

    Py_DECREF(fast);
    Py_DECREF(lines);
    Py_DECREF(lyrics);
    Py_DECREF(resolved);
    PyGILState_Release(gil);
    return 0;
}

int ls_lyrics_index_at(const LsLyrics *lyrics, double position) {
    if (!lyrics || lyrics->count <= 0 || !lyrics->synced) {
        return -1;
    }
    /* Last line whose timestamp is at or before `position`; -1 while the
     * track is still ahead of the first one, which is a real state (an
     * intro) and not the same as "no lyrics". */
    int found = -1;
    for (int i = 0; i < lyrics->count; i++) {
        const double when = lyrics->lines[i].time;
        if (when < 0.0) {
            continue;
        }
        if (when <= position) {
            found = i;
        } else {
            break;
        }
    }
    return found;
}
