/*
 *  Runtime Python integration for DOSBox-X.
 *
 *  This layer intentionally loads Python dynamically so DOSBox-X still runs
 *  normally when no virtual environment is present.
 */

#include "dosbox_python.h"

#include "control.h"
#include "logging.h"

#include <algorithm>
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

struct PythonAPI {
#if defined(WIN32) && !defined(HX_DOS)
	HMODULE dll = NULL;
#endif
	void (*Py_SetProgramName)(const wchar_t *) = NULL;
	void (*Py_Initialize)(void) = NULL;
	int (*Py_IsInitialized)(void) = NULL;
	const char *(*Py_GetVersion)(void) = NULL;
	int (*PyRun_SimpleStringFlags)(const char *, void *) = NULL;
	void *(*PyErr_Occurred)(void) = NULL;
	void (*PyErr_Print)(void) = NULL;
	int (*Py_FinalizeEx)(void) = NULL;

	bool loaded(void) const
	{
		return Py_SetProgramName && Py_Initialize && Py_IsInitialized &&
		       Py_GetVersion && PyRun_SimpleStringFlags && PyErr_Occurred &&
		       PyErr_Print && Py_FinalizeEx;
	}
};

struct PythonRuntime {
	PythonAPI api = {};
	bool initialized = false;
	std::string working_dir = {};
	std::string mods_dir = {};
	std::string venv_dir = {};
	std::string python_dll = {};
};

PythonRuntime g_python = {};

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

static std::string build_python_bootstrap(const std::string &mods_dir)
{
	const std::string escaped_mods_dir = escape_python_string(mods_dir);
	std::ostringstream script;
	script
		<< "import builtins\n"
		<< "import os\n"
		<< "import pathlib\n"
		<< "import runpy\n"
		<< "import sys\n"
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
		<< "        print(f'PYTHON: loading mod {mod_path.name}', flush=True)\n"
		<< "        runpy.run_path(str(mod_path), run_name=f'dosbox_mod_{mod_path.stem}')\n"
		<< "else:\n"
		<< "    print(f'PYTHON: mods directory not found: {mods_dir}', flush=True)\n"
		<< "sys.stdout.flush()\n";
	return script.str();
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
	g_python.api.PyErr_Occurred =
	        reinterpret_cast<void *(*)(void)>(
	                resolve_symbol(g_python.api.dll, "PyErr_Occurred"));
	g_python.api.PyErr_Print =
	        reinterpret_cast<void (*)(void)>(
	                resolve_symbol(g_python.api.dll, "PyErr_Print"));
	g_python.api.Py_FinalizeEx =
	        reinterpret_cast<int (*)(void)>(
	                resolve_symbol(g_python.api.dll, "Py_FinalizeEx"));

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

	if (!g_python.mods_dir.empty()) {
		const std::string bootstrap = build_python_bootstrap(g_python.mods_dir);
		if (g_python.api.PyRun_SimpleStringFlags(bootstrap.c_str(), NULL) != 0) {
			LOG_MSG("PYTHON: mod bootstrap failed");
			if (g_python.api.PyErr_Occurred && g_python.api.PyErr_Occurred() != NULL &&
			    g_python.api.PyErr_Print) {
				g_python.api.PyErr_Print();
			}
		}
	}

	return true;
#endif
}

void DOSBoxPython_Shutdown(void)
{
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
}
