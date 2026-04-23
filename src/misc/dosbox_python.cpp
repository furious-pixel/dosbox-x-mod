/*
 *  Runtime Python integration for DOSBox-X.
 *
 *  This layer intentionally loads Python dynamically so DOSBox-X still runs
 *  normally when no virtual environment is present.
 */

#include "dosbox_python.h"

#include "control.h"
#include "logging.h"
#include "mem.h"

#include <algorithm>
#include <cctype>
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
	struct ExecutableConfig {
		std::string name = {};
		std::string name_upper = {};
		std::string landmark_string = {};
		uint32_t landmark_reloc = 0;
		bool has_scan_range = false;
		uint32_t scan_start = 0;
		uint32_t scan_end = 0;
		bool delta_ready = false;
		int64_t delta = 0;
	};

	PythonAPI api = {};
	bool initialized = false;
	std::string working_dir = {};
	std::string mods_dir = {};
	std::string venv_dir = {};
	std::string python_dll = {};
	std::vector<ExecutableConfig> executable_configs = {};
	bool pending_scan = false;
	uint16_t pending_scan_handle = 0;
	size_t pending_scan_index = 0;
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

static std::string basename_of(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	if (pos == std::string::npos)
		return path;
	return path.substr(pos + 1);
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
		<< "        if mod_path.name.lower() == 'mod_init.py':\n"
		<< "            continue\n"
		<< "        print(f'PYTHON: loading mod {mod_path.name}', flush=True)\n"
		<< "        runpy.run_path(str(mod_path), run_name=f'dosbox_mod_{mod_path.stem}')\n"
		<< "else:\n"
		<< "    print(f'PYTHON: mods directory not found: {mods_dir}', flush=True)\n"
		<< "sys.stdout.flush()\n";
	return script.str();
}

static std::string strip_python_comments(const std::string &text)
{
	std::string result;
	result.reserve(text.size());

	bool in_string = false;
	char quote = '\0';
	bool escaped = false;

	for (size_t i = 0; i < text.size(); ++i) {
		const char ch = text[i];
		if (in_string) {
			result.push_back(ch);
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == quote) {
				in_string = false;
			}
			continue;
		}

		if (ch == '\'' || ch == '"') {
			in_string = true;
			quote = ch;
			escaped = false;
			result.push_back(ch);
			continue;
		}

		if (ch == '#') {
			while (i < text.size() && text[i] != '\n')
				++i;
			if (i < text.size())
				result.push_back(text[i]);
			continue;
		}

		result.push_back(ch);
	}

	return result;
}

static size_t skip_whitespace(const std::string &text, size_t pos)
{
	while (pos < text.size() &&
	       std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
		++pos;
	}
	return pos;
}

static bool parse_python_string(const std::string &text,
                                size_t start,
                                size_t *next_pos,
                                std::string *value)
{
	if (start >= text.size() || (text[start] != '\'' && text[start] != '"'))
		return false;

	const char quote = text[start];
	std::string result;
	bool escaped = false;
	size_t pos = start + 1;

	while (pos < text.size()) {
		const char ch = text[pos++];
		if (escaped) {
			switch (ch) {
			case 'n': result.push_back('\n'); break;
			case 'r': result.push_back('\r'); break;
			case 't': result.push_back('\t'); break;
			default: result.push_back(ch); break;
			}
			escaped = false;
			continue;
		}

		if (ch == '\\') {
			escaped = true;
			continue;
		}

		if (ch == quote) {
			if (next_pos)
				*next_pos = pos;
			if (value)
				*value = result;
			return true;
		}

		result.push_back(ch);
	}

	return false;
}

static bool find_key_value_start(const std::string &text,
                                 const std::string &key,
                                 size_t start,
                                 size_t *value_start)
{
	size_t pos = start;
	while (pos < text.size()) {
		if (text[pos] != '\'' && text[pos] != '"') {
			++pos;
			continue;
		}

		size_t next = pos;
		std::string parsed_key;
		if (!parse_python_string(text, pos, &next, &parsed_key))
			return false;

		pos = next;
		const size_t colon = skip_whitespace(text, pos);
		if (parsed_key == key && colon < text.size() && text[colon] == ':') {
			*value_start = skip_whitespace(text, colon + 1);
			return true;
		}
	}

	return false;
}

static bool extract_bracketed_block(const std::string &text,
                                    size_t start,
                                    char open_char,
                                    char close_char,
                                    std::string *block)
{
	if (start >= text.size() || text[start] != open_char)
		return false;

	size_t depth = 0;
	bool in_string = false;
	char quote = '\0';
	bool escaped = false;

	for (size_t pos = start; pos < text.size(); ++pos) {
		const char ch = text[pos];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == quote) {
				in_string = false;
			}
			continue;
		}

		if (ch == '\'' || ch == '"') {
			in_string = true;
			quote = ch;
			escaped = false;
			continue;
		}

		if (ch == open_char) {
			++depth;
			continue;
		}

		if (ch == close_char) {
			if (depth == 0)
				return false;
			--depth;
			if (depth == 0) {
				*block = text.substr(start, pos - start + 1);
				return true;
			}
		}
	}

	return false;
}

static bool extract_named_block(const std::string &text,
                                const std::string &key,
                                char open_char,
                                char close_char,
                                std::string *block)
{
	size_t value_start = 0;
	if (!find_key_value_start(text, key, 0, &value_start))
		return false;
	return extract_bracketed_block(text, value_start, open_char, close_char, block);
}

static bool extract_string_field(const std::string &text,
                                 const std::string &key,
                                 std::string *value)
{
	size_t value_start = 0;
	if (!find_key_value_start(text, key, 0, &value_start))
		return false;
	return parse_python_string(text, value_start, NULL, value);
}

static bool extract_uint32_field(const std::string &text,
                                 const std::string &key,
                                 uint32_t *value)
{
	size_t value_start = 0;
	if (!find_key_value_start(text, key, 0, &value_start))
		return false;

	size_t end = value_start;
	if (end < text.size() && (text[end] == '+' || text[end] == '-'))
		++end;

	while (end < text.size()) {
		const char ch = text[end];
		const bool hex_digit = (ch >= '0' && ch <= '9') ||
		                       (ch >= 'a' && ch <= 'f') ||
		                       (ch >= 'A' && ch <= 'F') ||
		                       ch == 'x' || ch == 'X';
		if (!hex_digit)
			break;
		++end;
	}

	if (end == value_start)
		return false;

	const std::string token = text.substr(value_start, end - value_start);
	char *parse_end = NULL;
	const unsigned long parsed = std::strtoul(token.c_str(), &parse_end, 0);
	if (!parse_end || *parse_end != '\0')
		return false;

	*value = static_cast<uint32_t>(parsed);
	return true;
}

static std::vector<std::string> split_top_level_dicts(const std::string &list_block)
{
	std::vector<std::string> blocks;
	if (list_block.size() < 2 || list_block.front() != '[' || list_block.back() != ']')
		return blocks;

	size_t object_start = std::string::npos;
	size_t depth = 0;
	bool in_string = false;
	char quote = '\0';
	bool escaped = false;

	for (size_t pos = 1; pos + 1 < list_block.size(); ++pos) {
		const char ch = list_block[pos];
		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == quote) {
				in_string = false;
			}
			continue;
		}

		if (ch == '\'' || ch == '"') {
			in_string = true;
			quote = ch;
			escaped = false;
			continue;
		}

		if (ch == '{') {
			if (depth == 0)
				object_start = pos;
			++depth;
			continue;
		}

		if (ch == '}') {
			if (depth == 0)
				continue;
			--depth;
			if (depth == 0 && object_start != std::string::npos) {
				blocks.push_back(list_block.substr(object_start, pos - object_start + 1));
				object_start = std::string::npos;
			}
		}
	}

	return blocks;
}

static bool load_mod_init_config(const std::string &mods_dir)
{
	g_python.executable_configs.clear();
	g_python.pending_scan = false;
	g_python.pending_scan_handle = 0;
	g_python.pending_scan_index = 0;

	if (mods_dir.empty() || !directory_exists(mods_dir))
		return false;

	const std::string mod_init_path = join_path(mods_dir, "mod_init.py");
	if (!path_exists(mod_init_path))
		return false;

	std::ifstream input(mod_init_path.c_str(), std::ios::in | std::ios::binary);
	if (!input) {
		LOG_MSG("MOD ERROR: failed to read %s", mod_init_path.c_str());
		return false;
	}

	std::ostringstream buffer;
	buffer << input.rdbuf();
	const std::string source = strip_python_comments(buffer.str());

	std::string executables_block;
	if (!extract_named_block(source, "executables", '[', ']', &executables_block)) {
		LOG_MSG("MOD ERROR: %s is missing MOD_INIT['executables']", mod_init_path.c_str());
		return false;
	}

	const std::vector<std::string> executable_blocks = split_top_level_dicts(executables_block);
	for (size_t i = 0; i < executable_blocks.size(); ++i) {
		const std::string &block = executable_blocks[i];
		PythonRuntime::ExecutableConfig config = {};
		std::string landmark_block;
		std::string scan_range_block;

		if (!extract_string_field(block, "name", &config.name) ||
		    !extract_named_block(block, "landmark", '{', '}', &landmark_block) ||
		    !extract_string_field(landmark_block, "string", &config.landmark_string) ||
		    !extract_uint32_field(landmark_block, "reloc", &config.landmark_reloc)) {
			LOG_MSG("MOD ERROR: invalid executable entry %u in %s",
			        static_cast<unsigned int>(i + 1), mod_init_path.c_str());
			continue;
		}

		config.name_upper = uppercase_ascii_copy(config.name);
		if (extract_named_block(block, "scan_range", '{', '}', &scan_range_block)) {
			if (extract_uint32_field(scan_range_block, "start", &config.scan_start) &&
			    extract_uint32_field(scan_range_block, "end", &config.scan_end)) {
				config.has_scan_range = true;
			} else {
				LOG_MSG("MOD ERROR: invalid scan_range for %s in %s",
				        config.name.c_str(), mod_init_path.c_str());
			}
		}

		g_python.executable_configs.push_back(config);
	}

	if (g_python.executable_configs.empty()) {
		LOG_MSG("MOD ERROR: no valid executable configs found in %s", mod_init_path.c_str());
		return false;
	}

	LOG_MSG("MOD: loaded mod_init.py with %u executable config(s)",
	        static_cast<unsigned int>(g_python.executable_configs.size()));
	return true;
}

static uint32_t clamp_scan_end(uint64_t address)
{
	if (address > 0xffffffffull)
		return 0xffffffffu;
	return static_cast<uint32_t>(address);
}

static bool find_landmark_address(const PythonRuntime::ExecutableConfig &config,
                                  uint32_t *found_address)
{
	if (config.landmark_string.empty())
		return false;

	const uint64_t total_bytes = static_cast<uint64_t>(MEM_TotalPages()) * MEM_PAGESIZE;
	const uint32_t max_address = clamp_scan_end(total_bytes);
	if (max_address == 0)
		return false;

	uint32_t scan_start = 0;
	uint32_t scan_end = max_address;
	if (config.has_scan_range) {
		scan_start = std::min(config.scan_start, max_address);
		scan_end = std::min(config.scan_end, max_address);
	}

	if (scan_start >= scan_end)
		return false;

	const size_t needle_size = config.landmark_string.size();
	if (needle_size == 0 || static_cast<uint64_t>(scan_start) + needle_size > scan_end)
		return false;

	for (uint32_t addr = scan_start;
	     static_cast<uint64_t>(addr) + needle_size <= scan_end;
	     ++addr) {
		if (mem_readb(addr) != static_cast<uint8_t>(config.landmark_string[0]))
			continue;

		bool matched = true;
		for (size_t i = 1; i < needle_size; ++i) {
			if (mem_readb(addr + static_cast<uint32_t>(i)) !=
			    static_cast<uint8_t>(config.landmark_string[i])) {
				matched = false;
				break;
			}
		}

		if (matched) {
			*found_address = addr;
			return true;
		}
	}

	return false;
}

static void run_pending_scan(PythonRuntime::ExecutableConfig &config)
{
	uint32_t found_address = 0;
	if (!find_landmark_address(config, &found_address)) {
		LOG_MSG("MOD ERROR: could not match %s landmark '%s'",
		        config.name.c_str(), config.landmark_string.c_str());
		return;
	}

	const int64_t previous_delta = config.delta;
	const bool had_previous_delta = config.delta_ready;
	const int64_t new_delta = static_cast<int64_t>(found_address) -
	                          static_cast<int64_t>(config.landmark_reloc);

	config.delta = new_delta;
	config.delta_ready = true;

	if (config.delta >= 0) {
		LOG_MSG("MOD: %s delta = 0x%08lX",
		        config.name.c_str(), static_cast<unsigned long>(config.delta));
	} else {
		const unsigned long magnitude =
		        static_cast<unsigned long>(-config.delta);
		LOG_MSG("MOD: %s delta = -0x%08lX",
		        config.name.c_str(), magnitude);
	}

	if (had_previous_delta && previous_delta != new_delta) {
		if (previous_delta >= 0 && new_delta >= 0) {
			LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to 0x%08lX",
			        config.name.c_str(),
			        static_cast<unsigned long>(previous_delta),
			        static_cast<unsigned long>(new_delta));
		} else if (previous_delta < 0 && new_delta < 0) {
			LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to -0x%08lX",
			        config.name.c_str(),
			        static_cast<unsigned long>(-previous_delta),
			        static_cast<unsigned long>(-new_delta));
		} else if (previous_delta < 0) {
			LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to 0x%08lX",
			        config.name.c_str(),
			        static_cast<unsigned long>(-previous_delta),
			        static_cast<unsigned long>(new_delta));
		} else {
			LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to -0x%08lX",
			        config.name.c_str(),
			        static_cast<unsigned long>(previous_delta),
			        static_cast<unsigned long>(-new_delta));
		}
	}
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

	if (!g_python.mods_dir.empty())
		load_mod_init_config(g_python.mods_dir);

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
	g_python.executable_configs.clear();
	g_python.pending_scan = false;
	g_python.pending_scan_handle = 0;
	g_python.pending_scan_index = 0;
}

void DOSBoxPython_OnOpenFile(const char *name, unsigned short handle)
{
	if (!g_python.initialized || !name || g_python.executable_configs.empty())
		return;

	const std::string basename_upper = uppercase_ascii_copy(basename_of(name));
	for (size_t i = 0; i < g_python.executable_configs.size(); ++i) {
		PythonRuntime::ExecutableConfig &config = g_python.executable_configs[i];
		if (basename_upper != config.name_upper)
			continue;

		g_python.pending_scan = true;
		g_python.pending_scan_handle = handle;
		g_python.pending_scan_index = i;
		LOG_MSG("MOD: armed %s on handle %u",
		        config.name.c_str(), static_cast<unsigned int>(handle));
		return;
	}
}

void DOSBoxPython_OnCloseFile(unsigned short handle)
{
	if (!g_python.initialized || !g_python.pending_scan)
		return;

	if (handle != g_python.pending_scan_handle) {
		return;
	}

	g_python.pending_scan = false;
	g_python.pending_scan_handle = 0;

	if (g_python.pending_scan_index >= g_python.executable_configs.size())
		return;

	run_pending_scan(g_python.executable_configs[g_python.pending_scan_index]);
}
