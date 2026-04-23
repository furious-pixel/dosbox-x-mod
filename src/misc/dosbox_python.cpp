/*
 *  Runtime Python integration for DOSBox-X.
 *
 *  This layer intentionally loads Python dynamically so DOSBox-X still runs
 *  normally when no virtual environment is present.
 */

#include "dosbox_python.h"

#include "control.h"
#include "joystick.h"
#include "logging.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(WIN32) && !defined(HX_DOS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

struct PyObject;
typedef intptr_t Py_ssize_t;
typedef PyObject *(*PyCFunction)(PyObject *, PyObject *);

#define DOSBOX_PY_METH_VARARGS 0x0001

struct PyMethodDef {
	const char *ml_name;
	PyCFunction ml_meth;
	int ml_flags;
	const char *ml_doc;
};

struct PythonAPI {
#if defined(WIN32) && !defined(HX_DOS)
	HMODULE dll = NULL;
#endif
	void (*Py_SetProgramName)(const wchar_t *) = NULL;
	void (*Py_Initialize)(void) = NULL;
	int (*Py_IsInitialized)(void) = NULL;
	const char *(*Py_GetVersion)(void) = NULL;
	int (*PyRun_SimpleStringFlags)(const char *, void *) = NULL;
	PyObject *(*PyImport_ImportModule)(const char *) = NULL;
	PyObject *(*PyImport_AddModule)(const char *) = NULL;
	PyObject *(*PyObject_GetAttrString)(PyObject *, const char *) = NULL;
	int (*PyObject_SetAttrString)(PyObject *, const char *, PyObject *) = NULL;
	PyObject *(*PyObject_CallFunctionObjArgs)(PyObject *, ...) = NULL;
	int (*PyCallable_Check)(PyObject *) = NULL;
	PyObject *(*PyCFunction_NewEx)(PyMethodDef *, PyObject *, PyObject *) = NULL;
	PyObject *(*PyMapping_GetItemString)(PyObject *, const char *) = NULL;
	int (*PyMapping_HasKeyString)(PyObject *, const char *) = NULL;
	Py_ssize_t (*PySequence_Size)(PyObject *) = NULL;
	PyObject *(*PySequence_GetItem)(PyObject *, Py_ssize_t) = NULL;
	Py_ssize_t (*PyTuple_Size)(PyObject *) = NULL;
	PyObject *(*PyTuple_GetItem)(PyObject *, Py_ssize_t) = NULL;
	PyObject *(*PyUnicode_FromString)(const char *) = NULL;
	const char *(*PyUnicode_AsUTF8)(PyObject *) = NULL;
	unsigned long (*PyLong_AsUnsignedLong)(PyObject *) = NULL;
	long (*PyLong_AsLong)(PyObject *) = NULL;
	PyObject *(*PyLong_FromUnsignedLong)(unsigned long) = NULL;
	PyObject *(*PyLong_FromUnsignedLongLong)(unsigned long long) = NULL;
	PyObject *(*PyLong_FromLong)(long) = NULL;
	PyObject *(*PyFloat_FromDouble)(double) = NULL;
	PyObject *(*PyTuple_New)(Py_ssize_t) = NULL;
	int (*PyTuple_SetItem)(PyObject *, Py_ssize_t, PyObject *) = NULL;
	PyObject *(*PyBytes_FromStringAndSize)(const char *, Py_ssize_t) = NULL;
	void (*Py_IncRef)(PyObject *) = NULL;
	void (*Py_DecRef)(PyObject *) = NULL;
	void *(*PyErr_Occurred)(void) = NULL;
	void (*PyErr_Clear)(void) = NULL;
	void (*PyErr_Print)(void) = NULL;
	void (*PyErr_SetString)(PyObject *, const char *) = NULL;
	int (*Py_FinalizeEx)(void) = NULL;

	PyObject *PyExc_RuntimeError = NULL;
	PyObject *PyExc_TypeError = NULL;
	PyObject *PyExc_ValueError = NULL;
	PyObject *PyExc_OverflowError = NULL;

	bool loaded(void) const
	{
		return Py_SetProgramName && Py_Initialize && Py_IsInitialized &&
		       Py_GetVersion && PyRun_SimpleStringFlags &&
		       PyImport_ImportModule && PyImport_AddModule &&
		       PyObject_GetAttrString && PyObject_SetAttrString &&
		       PyObject_CallFunctionObjArgs && PyCallable_Check &&
		       PyCFunction_NewEx && PyMapping_GetItemString &&
		       PyMapping_HasKeyString && PySequence_Size &&
		       PySequence_GetItem && PyTuple_Size && PyTuple_GetItem &&
		       PyUnicode_FromString && PyUnicode_AsUTF8 &&
		       PyLong_AsUnsignedLong && PyLong_AsLong &&
		       PyLong_FromUnsignedLong &&
		       PyLong_FromUnsignedLongLong && PyLong_FromLong &&
		       PyFloat_FromDouble && PyTuple_New && PyTuple_SetItem &&
		       PyBytes_FromStringAndSize && Py_IncRef && Py_DecRef &&
		       PyErr_Occurred && PyErr_Clear && PyErr_Print &&
		       PyErr_SetString && Py_FinalizeEx &&
		       PyExc_RuntimeError && PyExc_TypeError &&
		       PyExc_ValueError && PyExc_OverflowError;
	}
};

struct PythonHookRegistration {
	size_t hook_id = 0;
	std::string exe_name_upper = {};
	uint32_t reloc_eip = 0;
	std::string kind = {};
	std::string description = {};
	bool render_aware = false;
	bool enabled = true;
	PyObject *callback = NULL;
};

enum PythonRenderCallbackKind {
	PYTHON_RENDER_CALLBACK_NONE = 0,
	PYTHON_RENDER_CALLBACK_INIT = 1,
	PYTHON_RENDER_CALLBACK_COMPOSITOR = 2,
};

struct PythonRenderCallbackRegistration {
	PythonRenderCallbackKind kind = PYTHON_RENDER_CALLBACK_NONE;
	std::string description = {};
	bool enabled = true;
	PyObject *callback = NULL;
};

struct PythonRuntime {
	PythonAPI api = {};
	bool initialized = false;
	std::string working_dir = {};
	std::string mods_dir = {};
	std::string venv_dir = {};
	std::string python_dll = {};
	PyObject *mod_module = NULL;
	PyObject *modstate = NULL;
	PyObject *gamemem = NULL;
	PyObject *modgl = NULL;
	std::vector<PythonHookRegistration> hooks = {};
	PythonRenderCallbackRegistration init_callback = {};
	PythonRenderCallbackRegistration compositor_callback = {};
	uint64_t initialized_context_generation = 0;
	ModOpenGLState gl_state = {};
};

PythonRuntime g_python = {};

struct PyOwnedRef {
	explicit PyOwnedRef(PyObject *obj = NULL) : object(obj) {}
	~PyOwnedRef()
	{
		reset();
	}

	PyObject *get(void) const
	{
		return object;
	}

	void reset(PyObject *obj = NULL)
	{
		if (object && g_python.api.Py_DecRef)
			g_python.api.Py_DecRef(object);
		object = obj;
	}

	PyObject *release(void)
	{
		PyObject *result = object;
		object = NULL;
		return result;
	}

	operator bool(void) const
	{
		return object != NULL;
	}

private:
	PyOwnedRef(const PyOwnedRef &);
	PyOwnedRef &operator=(const PyOwnedRef &);

	PyObject *object = NULL;
};

static std::string trim_copy(const std::string &value)
{
	const std::string whitespace = " \t\r\n";
	const size_t begin = value.find_first_not_of(whitespace);
	if (begin == std::string::npos)
		return {};

	const size_t end = value.find_last_not_of(whitespace);
	return value.substr(begin, end - begin + 1);
}

static bool path_exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

static bool directory_exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static std::string dirname_of(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	if (pos == std::string::npos)
		return {};
	return path.substr(0, pos);
}

static std::string join_path(const std::string &lhs, const std::string &rhs)
{
	if (lhs.empty())
		return rhs;
	if (rhs.empty())
		return lhs;

	const char tail = lhs[lhs.size() - 1];
	if (tail == '/' || tail == '\\')
		return lhs + rhs;

#if defined(WIN32)
	return lhs + "\\" + rhs;
#else
	return lhs + "/" + rhs;
#endif
}

static std::string make_absolute_path(const std::string &path)
{
#if defined(WIN32) && !defined(HX_DOS)
	char buffer[_MAX_PATH] = {};
	if (_fullpath(buffer, path.c_str(), _MAX_PATH) != NULL)
		return std::string(buffer);
#endif
	return path;
}

static std::string read_pyvenv_value(const std::string &pyvenv_cfg_path,
                                     const std::string &wanted_key)
{
	std::ifstream input(pyvenv_cfg_path.c_str());
	if (!input)
		return {};

	for (std::string line; std::getline(input, line); ) {
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;

		std::string key = trim_copy(line.substr(0, equals));
		if (key != wanted_key)
			continue;

		return trim_copy(line.substr(equals + 1));
	}

	return {};
}

static std::string read_pyvenv_home(const std::string &pyvenv_cfg_path)
{
	return read_pyvenv_value(pyvenv_cfg_path, "home");
}

static std::string read_pyvenv_version_info(const std::string &pyvenv_cfg_path)
{
	return read_pyvenv_value(pyvenv_cfg_path, "version_info");
}

static std::string uppercase_ascii_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::toupper(ch));
	});
	return value;
}

static std::string lowercase_ascii_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

static std::string versioned_python_dll_name(const std::string &version_info)
{
	std::vector<int> parts;
	std::string current;

	for (const char ch : version_info) {
		if (ch >= '0' && ch <= '9') {
			current.push_back(ch);
		} else if (!current.empty()) {
			parts.push_back(std::atoi(current.c_str()));
			current.clear();
			if (parts.size() >= 2)
				break;
		}
	}

	if (!current.empty() && parts.size() < 2)
		parts.push_back(std::atoi(current.c_str()));

	if (parts.size() < 2)
		return {};

	std::ostringstream dll_name;
	dll_name << "python" << parts[0] << parts[1] << ".dll";
	return dll_name.str();
}

static std::string escape_python_string(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');

	std::string escaped;
	escaped.reserve(path.size() + 8u);

	for (const char ch : path) {
		if (ch == '\'' || ch == '\\')
			escaped.push_back('\\');
		escaped.push_back(ch);
	}

	return escaped;
}

static std::string build_mod_helper_bootstrap(void)
{
	std::ostringstream script;
	script
		<< "import mod\n"
		<< "class _DOSBoxModState(object):\n"
		<< "    def get_modjoystick_axes(self):\n"
		<< "        return mod._get_modjoystick_axes()\n"
		<< "    pass\n"
		<< "if not hasattr(mod, 'modstate'):\n"
		<< "    mod.modstate = _DOSBoxModState()\n"
		<< "mod.modstate.frame = 0\n"
		<< "mod.modstate.time = 0.0\n"
		<< "mod.modstate.frame_delta = 0.0\n"
		<< "class _DOSBoxGameMemory(object):\n"
		<< "    def read_u8(self, addr):\n"
		<< "        return mod._read_u8(addr)\n"
		<< "    def read_u16(self, addr):\n"
		<< "        return mod._read_u16(addr)\n"
		<< "    def read_u32(self, addr):\n"
		<< "        return mod._read_u32(addr)\n"
		<< "    def read_i32(self, addr):\n"
		<< "        return mod._read_i32(addr)\n"
		<< "    def read_bytes(self, addr, size):\n"
		<< "        return mod._read_bytes(addr, size)\n"
		<< "    def write_u8(self, addr, value):\n"
		<< "        return mod._write_u8(addr, value)\n"
		<< "    def write_u16(self, addr, value):\n"
		<< "        return mod._write_u16(addr, value)\n"
		<< "    def write_u32(self, addr, value):\n"
		<< "        return mod._write_u32(addr, value)\n"
		<< "    def write_i32(self, addr, value):\n"
		<< "        return mod._write_i32(addr, value)\n"
		<< "if not hasattr(mod, 'gamemem'):\n"
		<< "    mod.gamemem = _DOSBoxGameMemory()\n"
		<< "class _DOSBoxOpenGL(object):\n"
		<< "    pass\n"
		<< "if not hasattr(mod, 'modgl'):\n"
		<< "    mod.modgl = _DOSBoxOpenGL()\n"
		<< "mod.modgl.context_generation = 0\n"
		<< "mod.modgl.present_count = 0\n"
		<< "mod.modgl.backbuffer_width = 0\n"
		<< "mod.modgl.backbuffer_height = 0\n"
		<< "mod.modgl.backbuffer_framebuffer = 0\n"
		<< "mod.modgl.draw_width = 0\n"
		<< "mod.modgl.draw_height = 0\n"
		<< "mod.modgl.view_mode = 0\n"
		<< "mod.modgl.clip_x = 0\n"
		<< "mod.modgl.clip_y = 0\n"
		<< "mod.modgl.clip_w = 0\n"
		<< "mod.modgl.clip_h = 0\n"
		<< "mod.modgl.game_viewport_x = 0\n"
		<< "mod.modgl.game_viewport_y = 0\n"
		<< "mod.modgl.game_viewport_w = 0\n"
		<< "mod.modgl.game_viewport_h = 0\n"
		<< "mod.modgl.mod_viewport_x = 0\n"
		<< "mod.modgl.mod_viewport_y = 0\n"
		<< "mod.modgl.mod_viewport_w = 0\n"
		<< "mod.modgl.mod_viewport_h = 0\n"
		<< "mod.MOD_RENDER_VIEW_GAME_ONLY = 0\n"
		<< "mod.MOD_RENDER_VIEW_MOD_ONLY = 1\n"
		<< "mod.MOD_RENDER_VIEW_SIDE_BY_SIDE = 2\n"
		<< "def modhook(exe_name, eip, kind):\n"
		<< "    def decorator(func):\n"
		<< "        return mod._register_hook(exe_name, eip, kind, func)\n"
		<< "    return decorator\n"
		<< "mod.modhook = modhook\n";
	script
		<< "def modrenderhook(exe_name, eip, kind):\n"
		<< "    def decorator(func):\n"
		<< "        return mod._register_render_hook(exe_name, eip, kind, func)\n"
		<< "    return decorator\n"
		<< "mod.modrenderhook = modrenderhook\n";
	script
		<< "def modrender(kind):\n"
		<< "    def decorator(func):\n"
		<< "        return mod._register_render_callback(kind, func)\n"
		<< "    return decorator\n"
		<< "mod.modrender = modrender\n";
	return script.str();
}

static std::string build_modstate_reset_script(void)
{
	std::ostringstream script;
	script
		<< "import mod\n"
		<< "class _DOSBoxModState(object):\n"
		<< "    def get_modjoystick_axes(self):\n"
		<< "        return mod._get_modjoystick_axes()\n"
		<< "    pass\n"
		<< "mod.modstate = _DOSBoxModState()\n"
		<< "mod.modstate.frame = 0\n"
		<< "mod.modstate.time = 0.0\n"
		<< "mod.modstate.frame_delta = 0.0\n";
	return script.str();
}

static std::string build_mod_loader_script(const std::string &mods_dir)
{
	const std::string escaped_mods_dir = escape_python_string(mods_dir);
	std::ostringstream script;
	script
		<< "import builtins\n"
		<< "import os\n"
		<< "import pathlib\n"
		<< "import runpy\n"
		<< "import sys\n"
		<< "import traceback\n"
		<< "if os.name == 'nt':\n"
		<< "    try:\n"
		<< "        _dosbox_console = builtins.open('CONOUT$', 'w', encoding='utf-8', buffering=1)\n"
		<< "        sys.stdout = _dosbox_console\n"
		<< "        sys.stderr = _dosbox_console\n"
		<< "    except OSError:\n"
		<< "        pass\n"
		<< "mods_dir = pathlib.Path('" << escaped_mods_dir << "').resolve()\n"
		<< "if mods_dir.is_dir():\n"
		<< "    sys.path.insert(0, str(mods_dir))\n"
		<< "    for mod_path in sorted(mods_dir.glob('*.py')):\n"
		<< "        if mod_path.name.startswith('_'):\n"
		<< "            continue\n"
		<< "        if mod_path.name.lower() == 'mod_init.py':\n"
		<< "            continue\n"
		<< "        print(f'PYTHON: loading mod {mod_path.name}', flush=True)\n"
		<< "        try:\n"
		<< "            runpy.run_path(str(mod_path), run_name=f'dosbox_mod_{mod_path.stem}')\n"
		<< "        except Exception:\n"
		<< "            print(f'MOD ERROR: failed loading {mod_path.name}', flush=True)\n"
		<< "            traceback.print_exc()\n"
		<< "else:\n"
		<< "    print(f'PYTHON: mods directory not found: {mods_dir}', flush=True)\n"
		<< "sys.stdout.flush()\n";
	return script.str();
}

static void clear_registered_hooks(void)
{
	for (size_t i = 0; i < g_python.hooks.size(); ++i) {
		if (g_python.hooks[i].callback && g_python.api.Py_DecRef)
			g_python.api.Py_DecRef(g_python.hooks[i].callback);
	}
	g_python.hooks.clear();
}

static void clear_registered_render_callbacks(void)
{
	PythonRenderCallbackRegistration *callbacks[2] = {
	        &g_python.init_callback, &g_python.compositor_callback};

	for (size_t i = 0; i < 2; ++i) {
		if (callbacks[i]->callback && g_python.api.Py_DecRef)
			g_python.api.Py_DecRef(callbacks[i]->callback);
		*callbacks[i] = PythonRenderCallbackRegistration();
	}

	g_python.initialized_context_generation = 0;
}

static void clear_python_error(void)
{
	if (g_python.api.PyErr_Clear)
		g_python.api.PyErr_Clear();
}

static void log_python_exception(const char *context)
{
	if (context && *context)
		LOG_MSG("MOD ERROR: %s", context);

	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL) {
		if (g_python.api.PyErr_Print)
			g_python.api.PyErr_Print();
		else
			clear_python_error();
	}
}

static void set_python_error(PyObject *exception_type, const char *message)
{
	if (g_python.api.PyErr_SetString && exception_type && message)
		g_python.api.PyErr_SetString(exception_type, message);
}

static std::string append_context_index(const std::string &context, size_t index)
{
	std::ostringstream stream;
	stream << context << "[" << static_cast<unsigned int>(index) << "]";
	return stream.str();
}

static bool set_object_attr(PyObject *object, const char *name, PyObject *value)
{
	if (!object || !value)
		return false;

	if (g_python.api.PyObject_SetAttrString(object, name, value) != 0) {
		log_python_exception(("failed to set attribute " + std::string(name)).c_str());
		return false;
	}

	return true;
}

static bool set_object_attr_u64(PyObject *object, const char *name, uint64_t value)
{
	PyOwnedRef py_value(
	        g_python.api.PyLong_FromUnsignedLongLong(
	                static_cast<unsigned long long>(value)));
	if (!py_value) {
		log_python_exception(("failed to create integer for " + std::string(name)).c_str());
		return false;
	}

	return set_object_attr(object, name, py_value.get());
}

static bool set_object_attr_double(PyObject *object, const char *name, double value)
{
	PyOwnedRef py_value(g_python.api.PyFloat_FromDouble(value));
	if (!py_value) {
		log_python_exception(("failed to create float for " + std::string(name)).c_str());
		return false;
	}

	return set_object_attr(object, name, py_value.get());
}

static bool update_modgl_state_object(PyObject *object, const ModOpenGLState &state)
{
	if (!object)
		return false;

	return set_object_attr_u64(object, "context_generation", state.context_generation) &&
	       set_object_attr_u64(object, "present_count", state.present_count) &&
	       set_object_attr_u64(object, "backbuffer_width", state.backbuffer_width) &&
	       set_object_attr_u64(object, "backbuffer_height", state.backbuffer_height) &&
	       set_object_attr_u64(object, "backbuffer_framebuffer",
	                           state.backbuffer_framebuffer) &&
	       set_object_attr_u64(object, "draw_width", state.draw_width) &&
	       set_object_attr_u64(object, "draw_height", state.draw_height) &&
	       set_object_attr_u64(object, "view_mode", state.view_mode) &&
	       set_object_attr_u64(object, "clip_x", state.clip_x) &&
	       set_object_attr_u64(object, "clip_y", state.clip_y) &&
	       set_object_attr_u64(object, "clip_w", state.clip_w) &&
	       set_object_attr_u64(object, "clip_h", state.clip_h) &&
	       set_object_attr_u64(object, "game_viewport_x",
	                           state.game_viewport_x) &&
	       set_object_attr_u64(object, "game_viewport_y",
	                           state.game_viewport_y) &&
	       set_object_attr_u64(object, "game_viewport_w",
	                           state.game_viewport_w) &&
	       set_object_attr_u64(object, "game_viewport_h",
	                           state.game_viewport_h) &&
	       set_object_attr_u64(object, "mod_viewport_x",
	                           state.mod_viewport_x) &&
	       set_object_attr_u64(object, "mod_viewport_y",
	                           state.mod_viewport_y) &&
	       set_object_attr_u64(object, "mod_viewport_w",
	                           state.mod_viewport_w) &&
	       set_object_attr_u64(object, "mod_viewport_h",
	                           state.mod_viewport_h);
}

static PyObject *get_required_mapping_item(PyObject *mapping,
                                           const char *key,
                                           const std::string &context)
{
	const int has_key = g_python.api.PyMapping_HasKeyString(mapping, key);
	if (has_key == 1)
		return g_python.api.PyMapping_GetItemString(mapping, key);

	if (has_key == 0) {
		LOG_MSG("MOD ERROR: %s is missing key '%s'", context.c_str(), key);
		return NULL;
	}

	log_python_exception((context + " failed key lookup").c_str());
	return NULL;
}

static PyObject *get_optional_mapping_item(PyObject *mapping,
                                           const char *key,
                                           const std::string &context)
{
	const int has_key = g_python.api.PyMapping_HasKeyString(mapping, key);
	if (has_key == 1)
		return g_python.api.PyMapping_GetItemString(mapping, key);

	if (has_key < 0)
		log_python_exception((context + " failed optional key lookup").c_str());

	return NULL;
}

static bool py_object_to_string(PyObject *object,
                                const std::string &context,
                                std::string *value)
{
	const char *utf8 = g_python.api.PyUnicode_AsUTF8(object);
	if (!utf8) {
		log_python_exception((context + " expected a string").c_str());
		return false;
	}

	*value = utf8;
	return true;
}

static bool py_object_to_uint32(PyObject *object,
                                const std::string &context,
                                uint32_t *value)
{
	clear_python_error();
	const unsigned long parsed = g_python.api.PyLong_AsUnsignedLong(object);
	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL) {
		log_python_exception((context + " expected an integer").c_str());
		return false;
	}

	*value = static_cast<uint32_t>(parsed);
	return true;
}

static bool ensure_mods_dir_on_sys_path(const std::string &mods_dir)
{
	if (mods_dir.empty())
		return true;

	const std::string escaped_mods_dir = escape_python_string(mods_dir);
	std::ostringstream script;
	script
	        << "import pathlib\n"
	        << "import sys\n"
	        << "_dosbox_mods_dir = str(pathlib.Path('" << escaped_mods_dir << "').resolve())\n"
	        << "if _dosbox_mods_dir not in sys.path:\n"
	        << "    sys.path.insert(0, _dosbox_mods_dir)\n";

	if (g_python.api.PyRun_SimpleStringFlags(script.str().c_str(), NULL) != 0) {
		log_python_exception("failed to add mods directory to sys.path");
		return false;
	}

	return true;
}

static std::string get_callable_name(PyObject *callable)
{
	std::string module_name = {};
	std::string function_name = {};

	PyOwnedRef module_obj(g_python.api.PyObject_GetAttrString(callable, "__module__"));
	if (module_obj)
		py_object_to_string(module_obj.get(), "__module__", &module_name);
	else
		clear_python_error();

	PyOwnedRef name_obj(g_python.api.PyObject_GetAttrString(callable, "__name__"));
	if (name_obj)
		py_object_to_string(name_obj.get(), "__name__", &function_name);
	else
		clear_python_error();

	std::ostringstream description;
	if (!module_name.empty() && !function_name.empty())
		description << module_name << "." << function_name;
	else if (!function_name.empty())
		description << function_name;
	else
		description << "<python hook>";

	return description.str();
}

static std::string get_callable_description(PyObject *callable,
                                            const std::string &exe_name_upper,
                                            uint32_t reloc_eip)
{
	std::ostringstream description;
	description << get_callable_name(callable);
	description << " -> " << exe_name_upper
	            << ":0x" << std::hex << std::uppercase
	            << static_cast<unsigned long>(reloc_eip);
	return description.str();
}

static bool attach_module_function(PyObject *module,
                                   const char *name,
                                   PyMethodDef *method)
{
	PyOwnedRef function(g_python.api.PyCFunction_NewEx(method, module, NULL));
	if (!function) {
		log_python_exception(("failed to create Python callable " + std::string(name)).c_str());
		return false;
	}

	if (g_python.api.PyObject_SetAttrString(module, name, function.get()) != 0) {
		log_python_exception(("failed to attach Python callable " + std::string(name)).c_str());
		return false;
	}

	return true;
}

static bool py_tuple_get_uint32_arg(PyObject *args,
                                    Py_ssize_t index,
                                    unsigned long *value)
{
	PyObject *item = g_python.api.PyTuple_GetItem(args, index);
	if (!item) {
		set_python_error(g_python.api.PyExc_RuntimeError,
		                 "failed to read integer argument");
		return false;
	}

	clear_python_error();
	*value = g_python.api.PyLong_AsUnsignedLong(item);
	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL)
		return false;

	return true;
}

static bool py_tuple_get_int32_arg(PyObject *args,
                                   Py_ssize_t index,
                                   long *value)
{
	PyObject *item = g_python.api.PyTuple_GetItem(args, index);
	if (!item) {
		set_python_error(g_python.api.PyExc_RuntimeError,
		                 "failed to read integer argument");
		return false;
	}

	clear_python_error();
	*value = g_python.api.PyLong_AsLong(item);
	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL)
		return false;

	return true;
}

static PyObject *py_register_hook_common(PyObject *args,
                                         bool render_aware,
                                         const char *api_name)
{
	if (g_python.api.PyTuple_Size(args) != 4) {
		const std::string message =
		        std::string(api_name) + " expects (exe_name, eip, kind, func)";
		set_python_error(g_python.api.PyExc_TypeError, message.c_str());
		return NULL;
	}

	PyObject *exe_name_obj = g_python.api.PyTuple_GetItem(args, 0);
	PyObject *reloc_eip_obj = g_python.api.PyTuple_GetItem(args, 1);
	PyObject *kind_obj = g_python.api.PyTuple_GetItem(args, 2);
	PyObject *callable_obj = g_python.api.PyTuple_GetItem(args, 3);
	if (!exe_name_obj || !reloc_eip_obj || !kind_obj || !callable_obj) {
		set_python_error(g_python.api.PyExc_RuntimeError,
		                 "failed to read hook arguments");
		return NULL;
	}

	const char *exe_name_utf8 = g_python.api.PyUnicode_AsUTF8(exe_name_obj);
	if (!exe_name_utf8)
		return NULL;

	const char *kind_utf8 = g_python.api.PyUnicode_AsUTF8(kind_obj);
	if (!kind_utf8)
		return NULL;

	clear_python_error();
	const unsigned long reloc_eip_value =
	        g_python.api.PyLong_AsUnsignedLong(reloc_eip_obj);
	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL)
		return NULL;

	const std::string exe_name = exe_name_utf8;
	std::string kind = kind_utf8;
	const uint32_t reloc_eip = static_cast<uint32_t>(reloc_eip_value);

	kind = lowercase_ascii_copy(kind);
	if (kind != "call") {
		set_python_error(g_python.api.PyExc_ValueError,
		                 "only 'call' hooks are supported");
		return NULL;
	}

	if (!g_python.api.PyCallable_Check(callable_obj)) {
		set_python_error(g_python.api.PyExc_TypeError,
		                 "hook target must be callable");
		return NULL;
	}

	PythonHookRegistration hook = {};
	hook.hook_id = g_python.hooks.size();
	hook.exe_name_upper = uppercase_ascii_copy(exe_name);
	hook.reloc_eip = reloc_eip;
	hook.kind = kind;
	hook.description = get_callable_description(callable_obj,
	                                            hook.exe_name_upper,
	                                            hook.reloc_eip);
	hook.render_aware = render_aware;
	hook.enabled = true;
	hook.callback = callable_obj;
	g_python.api.Py_IncRef(hook.callback);
	g_python.hooks.push_back(hook);

	LOG_MSG("MOD: registered %s%s",
	        render_aware ? "render hook " : "hook ",
	        hook.description.c_str());

	g_python.api.Py_IncRef(callable_obj);
	return callable_obj;
}

static PyObject *py_register_hook(PyObject *, PyObject *args)
{
	return py_register_hook_common(args, false, "_register_hook");
}

static PyObject *py_register_render_hook(PyObject *, PyObject *args)
{
	return py_register_hook_common(args, true, "_register_render_hook");
}

static PyObject *py_register_render_callback(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError,
		                 "_register_render_callback expects (kind, func)");
		return NULL;
	}

	PyObject *kind_obj = g_python.api.PyTuple_GetItem(args, 0);
	PyObject *callable_obj = g_python.api.PyTuple_GetItem(args, 1);
	if (!kind_obj || !callable_obj) {
		set_python_error(g_python.api.PyExc_RuntimeError,
		                 "failed to read render callback arguments");
		return NULL;
	}

	const char *kind_utf8 = g_python.api.PyUnicode_AsUTF8(kind_obj);
	if (!kind_utf8)
		return NULL;

	if (!g_python.api.PyCallable_Check(callable_obj)) {
		set_python_error(g_python.api.PyExc_TypeError,
		                 "render callback target must be callable");
		return NULL;
	}

	std::string kind = lowercase_ascii_copy(kind_utf8);
	PythonRenderCallbackRegistration *slot = NULL;
	PythonRenderCallbackKind callback_kind = PYTHON_RENDER_CALLBACK_NONE;

	if (kind == "init") {
		slot = &g_python.init_callback;
		callback_kind = PYTHON_RENDER_CALLBACK_INIT;
	} else if (kind == "compositor") {
		slot = &g_python.compositor_callback;
		callback_kind = PYTHON_RENDER_CALLBACK_COMPOSITOR;
	} else {
		set_python_error(g_python.api.PyExc_ValueError,
		                 "render callback kind must be 'init' or 'compositor'");
		return NULL;
	}

	if (slot->callback) {
		set_python_error(g_python.api.PyExc_ValueError,
		                 "only one render callback may be registered per kind");
		return NULL;
	}

	slot->kind = callback_kind;
	slot->description = get_callable_name(callable_obj);
	slot->enabled = true;
	slot->callback = callable_obj;
	g_python.api.Py_IncRef(slot->callback);

	LOG_MSG("MOD: registered %s render callback %s",
	        kind.c_str(),
	        slot->description.c_str());

	g_python.api.Py_IncRef(callable_obj);
	return callable_obj;
}

static PyObject *py_get_modjoystick_axes(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 0) {
		set_python_error(g_python.api.PyExc_TypeError,
		                 "_get_modjoystick_axes expects no arguments");
		return NULL;
	}

	int16_t axis_values[max_modjoy_axes] = {};
	ModJoystick_ReadAxes(axis_values, max_modjoy_axes);

	PyOwnedRef axis_tuple(g_python.api.PyTuple_New(max_modjoy_axes));
	if (!axis_tuple) {
		log_python_exception("failed to allocate joystick axis tuple");
		return NULL;
	}

	for (Py_ssize_t i = 0; i < max_modjoy_axes; ++i) {
		PyOwnedRef axis_value(g_python.api.PyLong_FromLong(
		        static_cast<long>(axis_values[i])));
		if (!axis_value) {
			log_python_exception("failed to convert joystick axis value");
			return NULL;
		}

		if (g_python.api.PyTuple_SetItem(axis_tuple.get(), i, axis_value.get()) != 0) {
			log_python_exception("failed to populate joystick axis tuple");
			return NULL;
		}

		axis_value.release();
	}

	return axis_tuple.release();
}

static PyObject *py_read_u8(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 1) {
		set_python_error(g_python.api.PyExc_TypeError, "read_u8 expects (addr)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr))
		return NULL;

	uint8_t value = 0;
	if (!MOD_ReadMemoryU8(static_cast<uint32_t>(reloc_addr), &value)) {
		set_python_error(g_python.api.PyExc_RuntimeError, "read_u8 failed");
		return NULL;
	}

	return g_python.api.PyLong_FromUnsignedLong(static_cast<unsigned long>(value));
}

static PyObject *py_read_u16(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 1) {
		set_python_error(g_python.api.PyExc_TypeError, "read_u16 expects (addr)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr))
		return NULL;

	uint16_t value = 0;
	if (!MOD_ReadMemoryU16(static_cast<uint32_t>(reloc_addr), &value)) {
		set_python_error(g_python.api.PyExc_RuntimeError, "read_u16 failed");
		return NULL;
	}

	return g_python.api.PyLong_FromUnsignedLong(static_cast<unsigned long>(value));
}

static PyObject *py_read_u32(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 1) {
		set_python_error(g_python.api.PyExc_TypeError, "read_u32 expects (addr)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr))
		return NULL;

	uint32_t value = 0;
	if (!MOD_ReadMemoryU32(static_cast<uint32_t>(reloc_addr), &value)) {
		set_python_error(g_python.api.PyExc_RuntimeError, "read_u32 failed");
		return NULL;
	}

	return g_python.api.PyLong_FromUnsignedLong(static_cast<unsigned long>(value));
}

static PyObject *py_read_i32(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 1) {
		set_python_error(g_python.api.PyExc_TypeError, "read_i32 expects (addr)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr))
		return NULL;

	int32_t value = 0;
	if (!MOD_ReadMemoryI32(static_cast<uint32_t>(reloc_addr), &value)) {
		set_python_error(g_python.api.PyExc_RuntimeError, "read_i32 failed");
		return NULL;
	}

	return g_python.api.PyLong_FromLong(static_cast<long>(value));
}

static PyObject *py_read_bytes(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError,
		                 "read_bytes expects (addr, size)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	unsigned long size_value = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr) ||
	    !py_tuple_get_uint32_arg(args, 1, &size_value)) {
		return NULL;
	}

	if (size_value > static_cast<unsigned long>(INT32_MAX)) {
		set_python_error(g_python.api.PyExc_OverflowError,
		                 "read_bytes size is too large");
		return NULL;
	}

	const size_t size = static_cast<size_t>(size_value);
	std::vector<uint8_t> data(size);
	if (!MOD_ReadMemoryBlock(static_cast<uint32_t>(reloc_addr),
	                         data.empty() ? NULL : data.data(),
	                         data.size())) {
		set_python_error(g_python.api.PyExc_RuntimeError, "read_bytes failed");
		return NULL;
	}

	return g_python.api.PyBytes_FromStringAndSize(
	        data.empty() ? "" : reinterpret_cast<const char *>(data.data()),
	        static_cast<Py_ssize_t>(data.size()));
}

static PyObject *py_write_u8(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError, "write_u8 expects (addr, value)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	unsigned long value = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr) ||
	    !py_tuple_get_uint32_arg(args, 1, &value)) {
		return NULL;
	}

	if (value > 0xfful) {
		set_python_error(g_python.api.PyExc_OverflowError,
		                 "write_u8 value out of range");
		return NULL;
	}

	if (!MOD_WriteMemoryU8(static_cast<uint32_t>(reloc_addr),
	                       static_cast<uint8_t>(value))) {
		set_python_error(g_python.api.PyExc_RuntimeError, "write_u8 failed");
		return NULL;
	}

	PyObject *value_obj = g_python.api.PyTuple_GetItem(args, 1);
	g_python.api.Py_IncRef(value_obj);
	return value_obj;
}

static PyObject *py_write_u16(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError, "write_u16 expects (addr, value)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	unsigned long value = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr) ||
	    !py_tuple_get_uint32_arg(args, 1, &value)) {
		return NULL;
	}

	if (value > 0xfffful) {
		set_python_error(g_python.api.PyExc_OverflowError,
		                 "write_u16 value out of range");
		return NULL;
	}

	if (!MOD_WriteMemoryU16(static_cast<uint32_t>(reloc_addr),
	                        static_cast<uint16_t>(value))) {
		set_python_error(g_python.api.PyExc_RuntimeError, "write_u16 failed");
		return NULL;
	}

	PyObject *value_obj = g_python.api.PyTuple_GetItem(args, 1);
	g_python.api.Py_IncRef(value_obj);
	return value_obj;
}

static PyObject *py_write_u32(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError, "write_u32 expects (addr, value)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	unsigned long value = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr) ||
	    !py_tuple_get_uint32_arg(args, 1, &value)) {
		return NULL;
	}

	if (!MOD_WriteMemoryU32(static_cast<uint32_t>(reloc_addr),
	                        static_cast<uint32_t>(value))) {
		set_python_error(g_python.api.PyExc_RuntimeError, "write_u32 failed");
		return NULL;
	}

	PyObject *value_obj = g_python.api.PyTuple_GetItem(args, 1);
	g_python.api.Py_IncRef(value_obj);
	return value_obj;
}

static PyObject *py_write_i32(PyObject *, PyObject *args)
{
	if (g_python.api.PyTuple_Size(args) != 2) {
		set_python_error(g_python.api.PyExc_TypeError, "write_i32 expects (addr, value)");
		return NULL;
	}

	unsigned long reloc_addr = 0;
	long value = 0;
	if (!py_tuple_get_uint32_arg(args, 0, &reloc_addr) ||
	    !py_tuple_get_int32_arg(args, 1, &value)) {
		return NULL;
	}

	if (!MOD_WriteMemoryI32(static_cast<uint32_t>(reloc_addr),
	                        static_cast<int32_t>(value))) {
		set_python_error(g_python.api.PyExc_RuntimeError, "write_i32 failed");
		return NULL;
	}

	PyObject *value_obj = g_python.api.PyTuple_GetItem(args, 1);
	g_python.api.Py_IncRef(value_obj);
	return value_obj;
}

static bool ensure_mod_helper_module(void)
{
	if (g_python.mod_module && g_python.modstate && g_python.gamemem && g_python.modgl)
		return true;

	if (!g_python.api.PyImport_AddModule("mod")) {
		log_python_exception("failed to add Python helper module 'mod'");
		return false;
	}

	PyOwnedRef mod_module(g_python.api.PyImport_ImportModule("mod"));
	if (!mod_module) {
		log_python_exception("failed to import Python helper module 'mod'");
		return false;
	}

	static PyMethodDef register_hook_method = {
	        "_register_hook", py_register_hook, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef register_render_hook_method = {
	        "_register_render_hook", py_register_render_hook, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef register_render_callback_method = {
	        "_register_render_callback", py_register_render_callback,
	        DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef get_modjoystick_axes_method = {
	        "_get_modjoystick_axes", py_get_modjoystick_axes, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef read_u8_method = {
	        "_read_u8", py_read_u8, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef read_u16_method = {
	        "_read_u16", py_read_u16, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef read_u32_method = {
	        "_read_u32", py_read_u32, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef read_i32_method = {
	        "_read_i32", py_read_i32, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef read_bytes_method = {
	        "_read_bytes", py_read_bytes, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef write_u8_method = {
	        "_write_u8", py_write_u8, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef write_u16_method = {
	        "_write_u16", py_write_u16, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef write_u32_method = {
	        "_write_u32", py_write_u32, DOSBOX_PY_METH_VARARGS, NULL};
	static PyMethodDef write_i32_method = {
	        "_write_i32", py_write_i32, DOSBOX_PY_METH_VARARGS, NULL};

	if (!attach_module_function(mod_module.get(), "_register_hook", &register_hook_method) ||
	    !attach_module_function(mod_module.get(), "_register_render_hook",
	                            &register_render_hook_method) ||
	    !attach_module_function(mod_module.get(), "_register_render_callback",
	                            &register_render_callback_method) ||
	    !attach_module_function(mod_module.get(), "_get_modjoystick_axes",
	                            &get_modjoystick_axes_method) ||
	    !attach_module_function(mod_module.get(), "_read_u8", &read_u8_method) ||
	    !attach_module_function(mod_module.get(), "_read_u16", &read_u16_method) ||
	    !attach_module_function(mod_module.get(), "_read_u32", &read_u32_method) ||
	    !attach_module_function(mod_module.get(), "_read_i32", &read_i32_method) ||
	    !attach_module_function(mod_module.get(), "_read_bytes", &read_bytes_method) ||
	    !attach_module_function(mod_module.get(), "_write_u8", &write_u8_method) ||
	    !attach_module_function(mod_module.get(), "_write_u16", &write_u16_method) ||
	    !attach_module_function(mod_module.get(), "_write_u32", &write_u32_method) ||
	    !attach_module_function(mod_module.get(), "_write_i32", &write_i32_method)) {
		return false;
	}

	const std::string bootstrap = build_mod_helper_bootstrap();
	if (g_python.api.PyRun_SimpleStringFlags(bootstrap.c_str(), NULL) != 0) {
		log_python_exception("failed to bootstrap Python helper module");
		return false;
	}

	PyOwnedRef modstate(
	        g_python.api.PyObject_GetAttrString(mod_module.get(), "modstate"));
	PyOwnedRef gamemem(
	        g_python.api.PyObject_GetAttrString(mod_module.get(), "gamemem"));
	PyOwnedRef modgl(
	        g_python.api.PyObject_GetAttrString(mod_module.get(), "modgl"));
	if (!modstate || !gamemem || !modgl) {
		log_python_exception("failed to resolve mod.modstate, mod.gamemem, or mod.modgl");
		return false;
	}

	if (g_python.mod_module && g_python.api.Py_DecRef)
		g_python.api.Py_DecRef(g_python.mod_module);
	if (g_python.modstate && g_python.api.Py_DecRef)
		g_python.api.Py_DecRef(g_python.modstate);
	if (g_python.gamemem && g_python.api.Py_DecRef)
		g_python.api.Py_DecRef(g_python.gamemem);
	if (g_python.modgl && g_python.api.Py_DecRef)
		g_python.api.Py_DecRef(g_python.modgl);

	g_python.mod_module = mod_module.release();
	g_python.modstate = modstate.release();
	g_python.gamemem = gamemem.release();
	g_python.modgl = modgl.release();
	update_modgl_state_object(g_python.modgl, g_python.gl_state);
	return true;
}

static bool refresh_modstate_reference(void)
{
	if (!g_python.mod_module)
		return false;

	PyOwnedRef modstate(
	        g_python.api.PyObject_GetAttrString(g_python.mod_module, "modstate"));
	if (!modstate) {
		log_python_exception("failed to resolve refreshed mod.modstate");
		return false;
	}

	if (g_python.modstate && g_python.api.Py_DecRef)
		g_python.api.Py_DecRef(g_python.modstate);

	g_python.modstate = modstate.release();
	return true;
}

static bool load_mod_init_config(const std::string &mods_dir,
                                 std::vector<ModExecutableConfig> *configs)
{
	configs->clear();

	if (mods_dir.empty() || !directory_exists(mods_dir))
		return false;

	const std::string mod_init_path = join_path(mods_dir, "mod_init.py");
	if (!path_exists(mod_init_path))
		return false;

	if (!ensure_mods_dir_on_sys_path(mods_dir))
		return false;

	PyOwnedRef runpy_module(g_python.api.PyImport_ImportModule("runpy"));
	if (!runpy_module) {
		log_python_exception("failed to import runpy for mod_init.py");
		return false;
	}

	PyOwnedRef run_path(g_python.api.PyObject_GetAttrString(runpy_module.get(), "run_path"));
	if (!run_path) {
		log_python_exception("failed to resolve runpy.run_path");
		return false;
	}

	PyOwnedRef mod_init_path_obj(g_python.api.PyUnicode_FromString(mod_init_path.c_str()));
	if (!mod_init_path_obj) {
		log_python_exception("failed to convert mod_init.py path to Python string");
		return false;
	}

	PyOwnedRef mod_globals(g_python.api.PyObject_CallFunctionObjArgs(
	        run_path.get(), mod_init_path_obj.get(), NULL));
	if (!mod_globals) {
		log_python_exception(("failed to execute " + mod_init_path).c_str());
		return false;
	}

	PyOwnedRef mod_init(get_required_mapping_item(mod_globals.get(), "MOD_INIT", mod_init_path));
	if (!mod_init)
		return false;

	PyOwnedRef executables(
	        get_required_mapping_item(mod_init.get(), "executables", "MOD_INIT"));
	if (!executables)
		return false;

	clear_python_error();
	const Py_ssize_t executable_count =
	        g_python.api.PySequence_Size(executables.get());
	if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL) {
		log_python_exception("MOD_INIT['executables'] must be a sequence");
		return false;
	}

	for (Py_ssize_t i = 0; i < executable_count; ++i) {
		const std::string entry_context = append_context_index("MOD_INIT['executables']", static_cast<size_t>(i));
		ModExecutableConfig config = {};
		PyOwnedRef executable(g_python.api.PySequence_GetItem(executables.get(), i));
		if (!executable) {
			log_python_exception((entry_context + " could not be read").c_str());
			continue;
		}

		PyOwnedRef name(get_required_mapping_item(executable.get(), "name", entry_context));
		PyOwnedRef landmark(get_required_mapping_item(executable.get(), "landmark", entry_context));
		if (!name || !landmark)
			continue;

		PyOwnedRef landmark_string(
		        get_required_mapping_item(landmark.get(), "string", entry_context + "['landmark']"));
		PyOwnedRef landmark_reloc(
		        get_required_mapping_item(landmark.get(), "reloc", entry_context + "['landmark']"));
		if (!landmark_string || !landmark_reloc)
			continue;

		if (!py_object_to_string(name.get(), entry_context + "['name']", &config.name) ||
		    !py_object_to_string(landmark_string.get(),
		                         entry_context + "['landmark']['string']",
		                         &config.landmark_string) ||
		    !py_object_to_uint32(landmark_reloc.get(),
		                         entry_context + "['landmark']['reloc']",
		                         &config.landmark_reloc)) {
			continue;
		}

		config.name_upper = uppercase_ascii_copy(config.name);

		PyOwnedRef scan_range(
		        get_optional_mapping_item(executable.get(), "scan_range", entry_context));
		if (scan_range) {
			PyOwnedRef scan_start(
			        get_required_mapping_item(scan_range.get(), "start", entry_context + "['scan_range']"));
			PyOwnedRef scan_end(
			        get_required_mapping_item(scan_range.get(), "end", entry_context + "['scan_range']"));
			if (scan_start && scan_end &&
			    py_object_to_uint32(scan_start.get(),
			                        entry_context + "['scan_range']['start']",
			                        &config.scan_start) &&
			    py_object_to_uint32(scan_end.get(),
			                        entry_context + "['scan_range']['end']",
			                        &config.scan_end)) {
				config.has_scan_range = true;
			} else {
				LOG_MSG("MOD ERROR: invalid scan_range for %s in %s",
				        config.name.c_str(), mod_init_path.c_str());
			}
		}

		PyOwnedRef frame_start(
		        get_optional_mapping_item(executable.get(), "frame_start", entry_context));
		if (frame_start) {
			if (py_object_to_uint32(frame_start.get(),
			                        entry_context + "['frame_start']",
			                        &config.frame_start_reloc)) {
				config.has_frame_start = true;
			}
		}

		configs->push_back(config);
	}

	if (configs->empty()) {
		LOG_MSG("MOD ERROR: no valid executable configs found in %s", mod_init_path.c_str());
		return false;
	}

	LOG_MSG("MOD: loaded mod_init.py with %u executable config(s)",
	        static_cast<unsigned int>(configs->size()));
	return true;
}

#if defined(WIN32) && !defined(HX_DOS)
typedef DLL_DIRECTORY_COOKIE (WINAPI *AddDllDirectoryFunc)(PCWSTR);

static std::wstring wide_from_native(const std::string &text)
{
	if (text.empty())
		return {};

	const int needed = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, NULL, 0);
	if (needed <= 0)
		return {};

	std::wstring result(static_cast<size_t>(needed - 1), L'\0');
	MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &result[0], needed);
	return result;
}

static FARPROC resolve_symbol(HMODULE dll, const char *name)
{
	FARPROC symbol = GetProcAddress(dll, name);
	if (!symbol)
		LOG_MSG("PYTHON: missing symbol %s", name);
	return symbol;
}

static PyObject *resolve_exception_object(HMODULE dll, const char *name)
{
	FARPROC symbol = resolve_symbol(dll, name);
	if (!symbol)
		return NULL;
	return *reinterpret_cast<PyObject **>(reinterpret_cast<ULONG_PTR>(symbol));
}

static bool load_python_api(const std::string &dll_path)
{
	const std::wstring wide_dll_path = wide_from_native(dll_path);
	const std::string dll_dir = dirname_of(dll_path);
	const std::wstring wide_dll_dir = wide_from_native(dll_dir);

	HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
	if (kernel32) {
		const AddDllDirectoryFunc add_dll_directory =
		        reinterpret_cast<AddDllDirectoryFunc>(
		                GetProcAddress(kernel32, "AddDllDirectory"));
		if (add_dll_directory && !wide_dll_dir.empty())
			add_dll_directory(wide_dll_dir.c_str());
	}

	g_python.api.dll = LoadLibraryW(wide_dll_path.c_str());
	if (!g_python.api.dll) {
		LOG_MSG("PYTHON: failed to load %s", dll_path.c_str());
		return false;
	}

	g_python.api.Py_SetProgramName =
	        reinterpret_cast<void (*)(const wchar_t *)>(
	                resolve_symbol(g_python.api.dll, "Py_SetProgramName"));
	g_python.api.Py_Initialize =
	        reinterpret_cast<void (*)(void)>(
	                resolve_symbol(g_python.api.dll, "Py_Initialize"));
	g_python.api.Py_IsInitialized =
	        reinterpret_cast<int (*)(void)>(
	                resolve_symbol(g_python.api.dll, "Py_IsInitialized"));
	g_python.api.Py_GetVersion =
	        reinterpret_cast<const char *(*)(void)>(
	                resolve_symbol(g_python.api.dll, "Py_GetVersion"));
	g_python.api.PyRun_SimpleStringFlags =
	        reinterpret_cast<int (*)(const char *, void *)>(
	                resolve_symbol(g_python.api.dll, "PyRun_SimpleStringFlags"));
	g_python.api.PyImport_ImportModule =
	        reinterpret_cast<PyObject *(*)(const char *)>(
	                resolve_symbol(g_python.api.dll, "PyImport_ImportModule"));
	g_python.api.PyImport_AddModule =
	        reinterpret_cast<PyObject *(*)(const char *)>(
	                resolve_symbol(g_python.api.dll, "PyImport_AddModule"));
	g_python.api.PyObject_GetAttrString =
	        reinterpret_cast<PyObject *(*)(PyObject *, const char *)>(
	                resolve_symbol(g_python.api.dll, "PyObject_GetAttrString"));
	g_python.api.PyObject_SetAttrString =
	        reinterpret_cast<int (*)(PyObject *, const char *, PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyObject_SetAttrString"));
	g_python.api.PyObject_CallFunctionObjArgs =
	        reinterpret_cast<PyObject *(*)(PyObject *, ...)>(
	                resolve_symbol(g_python.api.dll, "PyObject_CallFunctionObjArgs"));
	g_python.api.PyCallable_Check =
	        reinterpret_cast<int (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyCallable_Check"));
	g_python.api.PyCFunction_NewEx =
	        reinterpret_cast<PyObject *(*)(PyMethodDef *, PyObject *, PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyCFunction_NewEx"));
	g_python.api.PyMapping_GetItemString =
	        reinterpret_cast<PyObject *(*)(PyObject *, const char *)>(
	                resolve_symbol(g_python.api.dll, "PyMapping_GetItemString"));
	g_python.api.PyMapping_HasKeyString =
	        reinterpret_cast<int (*)(PyObject *, const char *)>(
	                resolve_symbol(g_python.api.dll, "PyMapping_HasKeyString"));
	g_python.api.PySequence_Size =
	        reinterpret_cast<Py_ssize_t (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PySequence_Size"));
	g_python.api.PySequence_GetItem =
	        reinterpret_cast<PyObject *(*)(PyObject *, Py_ssize_t)>(
	                resolve_symbol(g_python.api.dll, "PySequence_GetItem"));
	g_python.api.PyTuple_Size =
	        reinterpret_cast<Py_ssize_t (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyTuple_Size"));
	g_python.api.PyTuple_GetItem =
	        reinterpret_cast<PyObject *(*)(PyObject *, Py_ssize_t)>(
	                resolve_symbol(g_python.api.dll, "PyTuple_GetItem"));
	g_python.api.PyUnicode_FromString =
	        reinterpret_cast<PyObject *(*)(const char *)>(
	                resolve_symbol(g_python.api.dll, "PyUnicode_FromString"));
	g_python.api.PyUnicode_AsUTF8 =
	        reinterpret_cast<const char *(*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyUnicode_AsUTF8"));
	g_python.api.PyLong_AsUnsignedLong =
	        reinterpret_cast<unsigned long (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyLong_AsUnsignedLong"));
	g_python.api.PyLong_AsLong =
	        reinterpret_cast<long (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyLong_AsLong"));
	g_python.api.PyLong_FromUnsignedLong =
	        reinterpret_cast<PyObject *(*)(unsigned long)>(
	                resolve_symbol(g_python.api.dll, "PyLong_FromUnsignedLong"));
	g_python.api.PyLong_FromUnsignedLongLong =
	        reinterpret_cast<PyObject *(*)(unsigned long long)>(
	                resolve_symbol(g_python.api.dll, "PyLong_FromUnsignedLongLong"));
	g_python.api.PyLong_FromLong =
	        reinterpret_cast<PyObject *(*)(long)>(
	                resolve_symbol(g_python.api.dll, "PyLong_FromLong"));
	g_python.api.PyFloat_FromDouble =
	        reinterpret_cast<PyObject *(*)(double)>(
	                resolve_symbol(g_python.api.dll, "PyFloat_FromDouble"));
	g_python.api.PyTuple_New =
	        reinterpret_cast<PyObject *(*)(Py_ssize_t)>(
	                resolve_symbol(g_python.api.dll, "PyTuple_New"));
	g_python.api.PyTuple_SetItem =
	        reinterpret_cast<int (*)(PyObject *, Py_ssize_t, PyObject *)>(
	                resolve_symbol(g_python.api.dll, "PyTuple_SetItem"));
	g_python.api.PyBytes_FromStringAndSize =
	        reinterpret_cast<PyObject *(*)(const char *, Py_ssize_t)>(
	                resolve_symbol(g_python.api.dll, "PyBytes_FromStringAndSize"));
	g_python.api.Py_IncRef =
	        reinterpret_cast<void (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "Py_IncRef"));
	g_python.api.Py_DecRef =
	        reinterpret_cast<void (*)(PyObject *)>(
	                resolve_symbol(g_python.api.dll, "Py_DecRef"));
	g_python.api.PyErr_Occurred =
	        reinterpret_cast<void *(*)(void)>(
	                resolve_symbol(g_python.api.dll, "PyErr_Occurred"));
	g_python.api.PyErr_Clear =
	        reinterpret_cast<void (*)(void)>(
	                resolve_symbol(g_python.api.dll, "PyErr_Clear"));
	g_python.api.PyErr_Print =
	        reinterpret_cast<void (*)(void)>(
	                resolve_symbol(g_python.api.dll, "PyErr_Print"));
	g_python.api.PyErr_SetString =
	        reinterpret_cast<void (*)(PyObject *, const char *)>(
	                resolve_symbol(g_python.api.dll, "PyErr_SetString"));
	g_python.api.Py_FinalizeEx =
	        reinterpret_cast<int (*)(void)>(
	                resolve_symbol(g_python.api.dll, "Py_FinalizeEx"));

	g_python.api.PyExc_RuntimeError =
	        resolve_exception_object(g_python.api.dll, "PyExc_RuntimeError");
	g_python.api.PyExc_TypeError =
	        resolve_exception_object(g_python.api.dll, "PyExc_TypeError");
	g_python.api.PyExc_ValueError =
	        resolve_exception_object(g_python.api.dll, "PyExc_ValueError");
	g_python.api.PyExc_OverflowError =
	        resolve_exception_object(g_python.api.dll, "PyExc_OverflowError");

	if (!g_python.api.loaded()) {
		FreeLibrary(g_python.api.dll);
		g_python.api = {};
		return false;
	}

	return true;
}
#endif

static bool discover_python_paths(const Config &config)
{
	const std::string working_dir = make_absolute_path(".");
	const std::string pyvenv_cfg = join_path(join_path(working_dir, ".venv"), "pyvenv.cfg");

	if (!path_exists(pyvenv_cfg))
		return false;

	const std::string venv_dir = dirname_of(pyvenv_cfg);
	const std::string python_home = read_pyvenv_home(pyvenv_cfg);
	const std::string version_info = read_pyvenv_version_info(pyvenv_cfg);
	if (python_home.empty())
		return false;

	g_python.working_dir = working_dir;
	g_python.mods_dir = config.opt_moddir.empty() ? "" : make_absolute_path(config.opt_moddir);
	g_python.venv_dir = venv_dir;

	const std::string versioned_dll = versioned_python_dll_name(version_info);
	if (!versioned_dll.empty()) {
		const std::string versioned_path = join_path(python_home, versioned_dll);
		if (path_exists(versioned_path))
			g_python.python_dll = versioned_path;
	}

	if (g_python.python_dll.empty())
		g_python.python_dll = join_path(python_home, "python3.dll");

	return true;
}

} // namespace

bool DOSBoxPython_Init(const Config& config)
{
	const bool python_requested = config.opt_python || !config.opt_moddir.empty();
	if (!python_requested)
		return false;

	if (g_python.initialized)
		return true;

#if !defined(WIN32) || defined(HX_DOS)
	LOG_MSG("PYTHON: runtime loading is currently implemented for Windows builds only");
	return false;
#else
	if (!discover_python_paths(config)) {
		LOG_MSG("PYTHON: no .venv/pyvenv.cfg found in working directory '%s'",
		        make_absolute_path(".").c_str());
		return false;
	}

	if (!path_exists(g_python.python_dll)) {
		LOG_MSG("PYTHON: expected DLL not found at %s", g_python.python_dll.c_str());
		return false;
	}

	if (!load_python_api(g_python.python_dll))
		return false;

	const std::wstring venv_python_exe =
	        wide_from_native(join_path(join_path(g_python.venv_dir, "Scripts"), "python.exe"));
	if (venv_python_exe.empty()) {
		LOG_MSG("PYTHON: failed to resolve venv python executable path");
		return false;
	}

	g_python.api.Py_SetProgramName(venv_python_exe.c_str());
	g_python.api.Py_Initialize();
	if (!g_python.api.Py_IsInitialized()) {
		LOG_MSG("PYTHON: interpreter failed to initialize");
		DOSBoxPython_Shutdown();
		return false;
	}

	g_python.initialized = true;

	LOG_MSG("PYTHON: initialized from %s", g_python.venv_dir.c_str());
	LOG_MSG("PYTHON: working directory %s", g_python.working_dir.c_str());
	if (!g_python.mods_dir.empty())
		LOG_MSG("PYTHON: mods directory %s", g_python.mods_dir.c_str());
	else
		LOG_MSG("PYTHON: mods disabled (no -moddir specified)");
	if (!config.opt_console)
		LOG_MSG("PYTHON: use -console to see Python print() output on Windows");

	const char *version = g_python.api.Py_GetVersion ? g_python.api.Py_GetVersion() : NULL;
	if (version)
		LOG_MSG("PYTHON: version %s", version);

	if (!ensure_mod_helper_module()) {
		DOSBoxPython_Shutdown();
		return false;
	}

	return true;
#endif
}

void DOSBoxPython_Shutdown(void)
{
	clear_registered_hooks();
	clear_registered_render_callbacks();

	if (g_python.mod_module && g_python.api.Py_DecRef) {
		g_python.api.Py_DecRef(g_python.mod_module);
		g_python.mod_module = NULL;
	}
	if (g_python.modstate && g_python.api.Py_DecRef) {
		g_python.api.Py_DecRef(g_python.modstate);
		g_python.modstate = NULL;
	}
	if (g_python.gamemem && g_python.api.Py_DecRef) {
		g_python.api.Py_DecRef(g_python.gamemem);
		g_python.gamemem = NULL;
	}
	if (g_python.modgl && g_python.api.Py_DecRef) {
		g_python.api.Py_DecRef(g_python.modgl);
		g_python.modgl = NULL;
	}

#if defined(WIN32) && !defined(HX_DOS)
	if (g_python.initialized && g_python.api.Py_FinalizeEx)
		g_python.api.Py_FinalizeEx();

	if (g_python.api.dll) {
		FreeLibrary(g_python.api.dll);
		g_python.api.dll = NULL;
	}
#endif

	g_python.api = {};
	g_python.initialized = false;
	g_python.working_dir.clear();
	g_python.mods_dir.clear();
	g_python.venv_dir.clear();
	g_python.python_dll.clear();
	g_python.gl_state = {};
}

bool DOSBoxPython_LoadModInitConfigs(std::vector<ModExecutableConfig> *configs)
{
	if (!configs || !g_python.initialized)
		return false;

	return load_mod_init_config(g_python.mods_dir, configs);
}

bool DOSBoxPython_LoadMods(std::vector<ModPythonHookRegistration> *hooks)
{
	if (!hooks || !g_python.initialized)
		return false;

	hooks->clear();
	clear_registered_hooks();
	clear_registered_render_callbacks();

	if (g_python.mods_dir.empty() || !directory_exists(g_python.mods_dir))
		return false;

	if (!ensure_mods_dir_on_sys_path(g_python.mods_dir) ||
	    !ensure_mod_helper_module()) {
		return false;
	}

	const std::string loader = build_mod_loader_script(g_python.mods_dir);
	if (g_python.api.PyRun_SimpleStringFlags(loader.c_str(), NULL) != 0) {
		log_python_exception("mod loader bootstrap failed");
		return false;
	}

	hooks->reserve(g_python.hooks.size());
	for (size_t i = 0; i < g_python.hooks.size(); ++i) {
		const PythonHookRegistration &registered = g_python.hooks[i];
		ModPythonHookRegistration hook = {};
		hook.hook_id = registered.hook_id;
		hook.exe_name_upper = registered.exe_name_upper;
		hook.reloc_eip = registered.reloc_eip;
		hook.kind = registered.kind;
		hook.description = registered.description;
		hook.render_aware = registered.render_aware;
		hooks->push_back(hook);
	}

	const unsigned int render_callback_count =
	        (g_python.init_callback.callback ? 1u : 0u) +
	        (g_python.compositor_callback.callback ? 1u : 0u);
	if (hooks->empty() && render_callback_count == 0u)
		LOG_MSG("MOD: no Python hooks or render callbacks registered");
	else
		LOG_MSG("MOD: loaded %u hook(s) and %u render callback(s)",
		        static_cast<unsigned int>(hooks->size()),
		        render_callback_count);

	return true;
}

void DOSBoxPython_ResetModRuntimeState(void)
{
	if (!g_python.initialized)
		return;

	if (!ensure_mod_helper_module())
		return;

	const std::string reset_script = build_modstate_reset_script();
	if (g_python.api.PyRun_SimpleStringFlags(reset_script.c_str(), NULL) != 0) {
		log_python_exception("failed to recreate Python modstate");
		return;
	}

	if (!refresh_modstate_reference())
		return;

	g_python.initialized_context_generation = 0;

	for (size_t i = 0; i < g_python.hooks.size(); ++i)
		g_python.hooks[i].enabled = true;
}

void DOSBoxPython_ResetModStateTiming(void)
{
	if (!g_python.initialized || !g_python.modstate)
		return;

	set_object_attr_u64(g_python.modstate, "frame", 0);
	set_object_attr_double(g_python.modstate, "time", 0.0);
	set_object_attr_double(g_python.modstate, "frame_delta", 0.0);
}

bool DOSBoxPython_UpdateModStateTiming(const ModFrameState &state)
{
	if (!g_python.initialized || !g_python.modstate)
		return false;

	return set_object_attr_u64(g_python.modstate, "frame", state.frame) &&
	       set_object_attr_double(g_python.modstate, "time", state.time_seconds) &&
	       set_object_attr_double(g_python.modstate, "frame_delta",
	                              state.frame_delta_seconds);
}

bool DOSBoxPython_InvokeHook(size_t hook_id)
{
	if (!g_python.initialized || !g_python.modstate || !g_python.gamemem)
		return false;
	if (hook_id >= g_python.hooks.size())
		return false;

	PythonHookRegistration &hook = g_python.hooks[hook_id];
	if (!hook.enabled || !hook.callback)
		return false;

	if (hook.render_aware) {
		if (!g_python.modgl)
			return false;
		if (!update_modgl_state_object(g_python.modgl, g_python.gl_state))
			return false;
	}

	PyOwnedRef result(hook.render_aware
	                         ? g_python.api.PyObject_CallFunctionObjArgs(
	                                   hook.callback,
	                                   g_python.modstate,
	                                   g_python.gamemem,
	                                   g_python.modgl,
	                                   NULL)
	                         : g_python.api.PyObject_CallFunctionObjArgs(
	                                   hook.callback,
	                                   g_python.modstate,
	                                   g_python.gamemem,
	                                   NULL));
	if (!result) {
		hook.enabled = false;
		log_python_exception(("disabling hook after exception: " + hook.description).c_str());
		return false;
	}

	return true;
}

void DOSBoxPython_NotifyOpenGLContextCreated(uint64_t context_generation)
{
	g_python.gl_state.context_generation = context_generation;
	g_python.gl_state.present_count = 0;
	g_python.initialized_context_generation = 0;

	if (!g_python.initialized || !g_python.modgl)
		return;

	update_modgl_state_object(g_python.modgl, g_python.gl_state);
}

static bool invoke_python_render_callback(PythonRenderCallbackRegistration *callback,
                                          const char *disable_context,
                                          PyObject *arg0,
                                          PyObject *arg1,
                                          PyObject *arg2,
                                          PyObject *arg3,
                                          PyObject *arg4,
                                          PyObject *arg5,
                                          PyObject *arg6)
{
	if (!callback || !callback->enabled || !callback->callback)
		return false;

	PyOwnedRef result(g_python.api.PyObject_CallFunctionObjArgs(
	        callback->callback, arg0, arg1, arg2, arg3, arg4, arg5, arg6, NULL));
	if (!result) {
		callback->enabled = false;
		log_python_exception((std::string("disabling render callback after exception: ") +
		                      disable_context + " -> " + callback->description).c_str());
		return false;
	}

	return true;
}

bool DOSBoxPython_InvokeOpenGLInitCallback(const ModOpenGLState &state)
{
	if (!g_python.initialized || !g_python.modstate)
		return false;
	if (!g_python.init_callback.callback)
		return false;
	if (!ensure_mod_helper_module())
		return false;

	g_python.gl_state = state;
	if (!update_modgl_state_object(g_python.modgl, g_python.gl_state))
		return false;
	if (g_python.initialized_context_generation == state.context_generation)
		return false;

	PyOwnedRef viewport_width(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_w));
	PyOwnedRef viewport_height(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_h));
	if (!viewport_width || !viewport_height) {
		log_python_exception("failed to build init callback viewport arguments");
		return false;
	}

	const bool invoked = invoke_python_render_callback(
	        &g_python.init_callback,
	        "init",
	        g_python.modstate,
	        g_python.modgl,
	        viewport_width.get(),
	        viewport_height.get(),
	        NULL,
	        NULL,
	        NULL);
	if (invoked)
		g_python.initialized_context_generation = state.context_generation;

	return invoked;
}

bool DOSBoxPython_InvokeOpenGLCompositorCallback(const ModOpenGLState &state)
{
	if (!g_python.initialized || !g_python.modstate)
		return false;
	if (!g_python.compositor_callback.callback)
		return false;
	if (!ensure_mod_helper_module())
		return false;

	g_python.gl_state = state;
	if (!update_modgl_state_object(g_python.modgl, g_python.gl_state))
		return false;

	PyOwnedRef viewport_x(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_x));
	PyOwnedRef viewport_y(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_y));
	PyOwnedRef viewport_width(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_w));
	PyOwnedRef viewport_height(
	        g_python.api.PyLong_FromUnsignedLong(state.mod_viewport_h));
	PyOwnedRef backbuffer_framebuffer(g_python.api.PyLong_FromUnsignedLong(
	        state.backbuffer_framebuffer));
	if (!viewport_x || !viewport_y || !viewport_width || !viewport_height ||
	    !backbuffer_framebuffer) {
		log_python_exception("failed to build compositor callback arguments");
		return false;
	}

	return invoke_python_render_callback(&g_python.compositor_callback,
	                                     "compositor",
	                                     g_python.modstate,
	                                     g_python.modgl,
	                                     viewport_x.get(),
	                                     viewport_y.get(),
	                                     viewport_width.get(),
	                                     viewport_height.get(),
	                                     backbuffer_framebuffer.get());
}
