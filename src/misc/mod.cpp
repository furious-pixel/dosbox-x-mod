#include "mod.h"

#include "control.h"
#include "dosbox_python.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "timer.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>

#if defined(WIN32) && !defined(HX_DOS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

extern int CPU_IsDynamicCore(void);
#if (C_DYNAMIC_X86)
extern void CPU_Core_Dyn_X86_Cache_Reset(void);
#endif
#if (C_DYNREC)
extern void CPU_Core_Dynrec_Cache_Reset(void);
#endif

namespace {

struct ModHookRuntime {
	size_t python_hook_id = 0;
	uint32_t reloc_eip = 0;
	uint32_t linear_eip = 0;
	std::string description = {};
};

struct ModExecutableRuntime {
	ModExecutableConfig config = {};
	bool delta_ready = false;
	int64_t delta = 0;
	bool frame_start_ready = false;
	uint32_t frame_start_linear = 0;
	bool frame_started = false;
	Uint64 frame_counter_start = 0;
	Uint64 frame_counter_last = 0;
	ModFrameState frame_state = {};
	std::vector<ModHookRuntime> hooks = {};
	std::unordered_map<uint32_t, std::vector<size_t> > hooks_by_linear = {};
};

struct ModRuntime {
	bool initialized = false;
	bool pending_scan = false;
	uint16_t pending_scan_handle = 0;
	size_t pending_scan_index = 0;
	bool active_executable_valid = false;
	size_t active_executable_index = 0;
	bool fast_enabled = false;
	std::vector<ModExecutableRuntime> executables = {};
};

ModRuntime g_mod = {};

static std::string uppercase_ascii_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::toupper(ch));
	});
	return value;
}

static std::string basename_of(const std::string &path)
{
	const size_t pos = path.find_last_of("/\\");
	if (pos == std::string::npos)
		return path;
	return path.substr(pos + 1);
}

static uint32_t clamp_scan_end(uint64_t address)
{
	if (address > 0xffffffffull)
		return 0xffffffffu;
	return static_cast<uint32_t>(address);
}

static bool apply_delta(uint32_t reloc, int64_t delta, uint32_t *linear)
{
	const int64_t value = static_cast<int64_t>(reloc) + delta;
	if (value < 0 || value > 0xffffffffll)
		return false;

	*linear = static_cast<uint32_t>(value);
	return true;
}

static bool get_active_runtime(ModExecutableRuntime **runtime)
{
	if (!g_mod.active_executable_valid)
		return false;
	if (g_mod.active_executable_index >= g_mod.executables.size())
		return false;

	ModExecutableRuntime &active = g_mod.executables[g_mod.active_executable_index];
	if (!active.delta_ready)
		return false;

	*runtime = &active;
	return true;
}

static void update_fast_enabled(void)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime)) {
		g_mod.fast_enabled = false;
		return;
	}

	g_mod.fast_enabled = runtime->frame_start_ready || !runtime->hooks_by_linear.empty();
}

static void reset_runtime_frame_state(ModExecutableRuntime &runtime)
{
	runtime.frame_started = false;
	runtime.frame_counter_start = 0;
	runtime.frame_counter_last = 0;
	runtime.frame_state = {};
	DOSBoxPython_ResetModStateTiming();
}

static void refresh_dynamic_cpu_cache(void)
{
	switch (CPU_IsDynamicCore()) {
#if (C_DYNAMIC_X86)
	case 1:
		CPU_Core_Dyn_X86_Cache_Reset();
		break;
#endif
#if (C_DYNREC)
	case 2:
		CPU_Core_Dynrec_Cache_Reset();
		break;
#endif
	default:
		break;
	}
}

static void rebuild_runtime_hooks(ModExecutableRuntime &runtime)
{
	runtime.frame_start_ready = false;
	runtime.frame_start_linear = 0;
	runtime.hooks_by_linear.clear();

	if (!runtime.delta_ready)
		return;

	if (runtime.config.has_frame_start) {
		uint32_t frame_start_linear = 0;
		if (apply_delta(runtime.config.frame_start_reloc, runtime.delta, &frame_start_linear)) {
			runtime.frame_start_linear = frame_start_linear;
			runtime.frame_start_ready = true;
		} else {
			LOG_MSG("MOD ERROR: %s frame_start 0x%08lX is invalid with delta",
			        runtime.config.name.c_str(),
			        static_cast<unsigned long>(runtime.config.frame_start_reloc));
		}
	}

	for (size_t i = 0; i < runtime.hooks.size(); ++i) {
		ModHookRuntime &hook = runtime.hooks[i];
		uint32_t linear_eip = 0;
		if (!apply_delta(hook.reloc_eip, runtime.delta, &linear_eip)) {
			LOG_MSG("MOD ERROR: hook %s has invalid runtime address",
			        hook.description.c_str());
			continue;
		}

		hook.linear_eip = linear_eip;
		runtime.hooks_by_linear[linear_eip].push_back(i);
	}
}

static bool find_landmark_address(const ModExecutableConfig &config,
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

static void log_delta_change(const ModExecutableRuntime &runtime,
                             int64_t previous_delta,
                             int64_t new_delta)
{
	if (previous_delta >= 0 && new_delta >= 0) {
		LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to 0x%08lX",
		        runtime.config.name.c_str(),
		        static_cast<unsigned long>(previous_delta),
		        static_cast<unsigned long>(new_delta));
	} else if (previous_delta < 0 && new_delta < 0) {
		LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to -0x%08lX",
		        runtime.config.name.c_str(),
		        static_cast<unsigned long>(-previous_delta),
		        static_cast<unsigned long>(-new_delta));
	} else if (previous_delta < 0) {
		LOG_MSG("MOD ERROR: %s delta changed from -0x%08lX to 0x%08lX",
		        runtime.config.name.c_str(),
		        static_cast<unsigned long>(-previous_delta),
		        static_cast<unsigned long>(new_delta));
	} else {
		LOG_MSG("MOD ERROR: %s delta changed from 0x%08lX to -0x%08lX",
		        runtime.config.name.c_str(),
		        static_cast<unsigned long>(previous_delta),
		        static_cast<unsigned long>(-new_delta));
	}
}

static void run_pending_scan(ModExecutableRuntime &runtime, size_t runtime_index)
{
	uint32_t found_address = 0;
	if (!find_landmark_address(runtime.config, &found_address)) {
		LOG_MSG("MOD ERROR: could not match %s landmark '%s'",
		        runtime.config.name.c_str(), runtime.config.landmark_string.c_str());
		if (g_mod.active_executable_valid &&
		    g_mod.active_executable_index == runtime_index) {
			g_mod.active_executable_valid = false;
			update_fast_enabled();
			DOSBoxPython_ResetModStateTiming();
		}
		return;
	}

	const int64_t previous_delta = runtime.delta;
	const bool had_previous_delta = runtime.delta_ready;
	const int64_t new_delta = static_cast<int64_t>(found_address) -
	                          static_cast<int64_t>(runtime.config.landmark_reloc);

	runtime.delta = new_delta;
	runtime.delta_ready = true;
	rebuild_runtime_hooks(runtime);
	reset_runtime_frame_state(runtime);
	refresh_dynamic_cpu_cache();

	g_mod.active_executable_index = runtime_index;
	g_mod.active_executable_valid = true;
	update_fast_enabled();

	if (runtime.delta >= 0) {
		LOG_MSG("MOD: %s delta = 0x%08lX",
		        runtime.config.name.c_str(), static_cast<unsigned long>(runtime.delta));
	} else {
		const unsigned long magnitude =
		        static_cast<unsigned long>(-runtime.delta);
		LOG_MSG("MOD: %s delta = -0x%08lX",
		        runtime.config.name.c_str(), magnitude);
	}

	if (had_previous_delta && previous_delta != new_delta)
		log_delta_change(runtime, previous_delta, new_delta);
}

static bool update_frame_timing(ModExecutableRuntime &runtime)
{
#if defined(WIN32) && !defined(HX_DOS)
	LARGE_INTEGER frequency = {};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return false;

	LARGE_INTEGER now = {};
	if (!QueryPerformanceCounter(&now))
		return false;

	if (!runtime.frame_started) {
		runtime.frame_started = true;
		runtime.frame_counter_start = static_cast<Uint64>(now.QuadPart);
		runtime.frame_counter_last = static_cast<Uint64>(now.QuadPart);
		runtime.frame_state.frame = 1;
		runtime.frame_state.time_seconds = 0.0;
		runtime.frame_state.frame_delta_seconds = 0.0;
	} else {
		runtime.frame_state.frame += 1;
		runtime.frame_state.frame_delta_seconds =
		        static_cast<double>(static_cast<Uint64>(now.QuadPart) - runtime.frame_counter_last) /
		        static_cast<double>(frequency.QuadPart);
		runtime.frame_state.time_seconds =
		        static_cast<double>(static_cast<Uint64>(now.QuadPart) - runtime.frame_counter_start) /
		        static_cast<double>(frequency.QuadPart);
		runtime.frame_counter_last = static_cast<Uint64>(now.QuadPart);
	}

	return DOSBoxPython_UpdateModStateTiming(runtime.frame_state);
#else
	const uint32_t now = GetTicks();
	if (!runtime.frame_started) {
		runtime.frame_started = true;
		runtime.frame_counter_start = now;
		runtime.frame_counter_last = now;
		runtime.frame_state.frame = 1;
		runtime.frame_state.time_seconds = 0.0;
		runtime.frame_state.frame_delta_seconds = 0.0;
	} else {
		runtime.frame_state.frame += 1;
		runtime.frame_state.frame_delta_seconds =
		        static_cast<double>(now - runtime.frame_counter_last) / 1000.0;
		runtime.frame_state.time_seconds =
		        static_cast<double>(now - runtime.frame_counter_start) / 1000.0;
		runtime.frame_counter_last = now;
	}

	return DOSBoxPython_UpdateModStateTiming(runtime.frame_state);
#endif
}

static bool translate_reloc_address(uint32_t reloc_addr, uint32_t *linear_addr)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return false;

	return apply_delta(reloc_addr, runtime->delta, linear_addr);
}

static void attach_python_hooks(const std::vector<ModPythonHookRegistration> &hooks)
{
	for (size_t i = 0; i < hooks.size(); ++i) {
		const ModPythonHookRegistration &hook = hooks[i];
		if (hook.kind != "call") {
			LOG_MSG("MOD ERROR: unsupported hook kind '%s' for %s",
			        hook.kind.c_str(), hook.description.c_str());
			continue;
		}

		bool matched_executable = false;
		for (size_t j = 0; j < g_mod.executables.size(); ++j) {
			ModExecutableRuntime &runtime = g_mod.executables[j];
			if (runtime.config.name_upper != hook.exe_name_upper)
				continue;

			ModHookRuntime runtime_hook = {};
			runtime_hook.python_hook_id = hook.hook_id;
			runtime_hook.reloc_eip = hook.reloc_eip;
			runtime_hook.description = hook.description;
			runtime.hooks.push_back(runtime_hook);
			matched_executable = true;
			break;
		}

		if (!matched_executable) {
			LOG_MSG("MOD ERROR: hook %s references unknown executable %s",
			        hook.description.c_str(), hook.exe_name_upper.c_str());
		}
	}
}

} // namespace

bool MOD_Init(const Config& config)
{
	g_mod = {};

	if (!DOSBoxPython_Init(config))
		return false;

	std::vector<ModExecutableConfig> configs;
	if (DOSBoxPython_LoadModInitConfigs(&configs)) {
		g_mod.executables.reserve(configs.size());
		for (size_t i = 0; i < configs.size(); ++i) {
			ModExecutableRuntime runtime = {};
			runtime.config = configs[i];
			g_mod.executables.push_back(runtime);
		}
	}

	std::vector<ModPythonHookRegistration> hooks;
	if (DOSBoxPython_LoadMods(&hooks))
		attach_python_hooks(hooks);

	g_mod.initialized = true;
	return true;
}

void MOD_Shutdown(void)
{
	g_mod = {};
	DOSBoxPython_Shutdown();
}

void MOD_OnOpenFile(const char *name, unsigned short handle)
{
	if (!g_mod.initialized || !name || g_mod.executables.empty())
		return;

	const std::string basename_upper = uppercase_ascii_copy(basename_of(name));
	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		ModExecutableRuntime &runtime = g_mod.executables[i];
		if (basename_upper != runtime.config.name_upper)
			continue;

		g_mod.pending_scan = true;
		g_mod.pending_scan_handle = handle;
		g_mod.pending_scan_index = i;
		if (g_mod.active_executable_valid &&
		    g_mod.active_executable_index == i) {
			g_mod.active_executable_valid = false;
			update_fast_enabled();
			DOSBoxPython_ResetModRuntimeState();
		}
		LOG_MSG("MOD: armed %s on handle %u",
		        runtime.config.name.c_str(), static_cast<unsigned int>(handle));
		return;
	}
}

void MOD_OnCloseFile(unsigned short handle)
{
	if (!g_mod.initialized || !g_mod.pending_scan)
		return;

	if (handle != g_mod.pending_scan_handle)
		return;

	g_mod.pending_scan = false;
	g_mod.pending_scan_handle = 0;

	if (g_mod.pending_scan_index >= g_mod.executables.size())
		return;

	run_pending_scan(g_mod.executables[g_mod.pending_scan_index],
	                g_mod.pending_scan_index);
}

bool MOD_FastEnabled(void)
{
	return g_mod.fast_enabled;
}

void MOD_OnCallsite(uint32_t linear_eip)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return;

	if (runtime->frame_start_ready && linear_eip == runtime->frame_start_linear)
		update_frame_timing(*runtime);

	const std::unordered_map<uint32_t, std::vector<size_t> >::const_iterator it =
	        runtime->hooks_by_linear.find(linear_eip);
	if (it == runtime->hooks_by_linear.end())
		return;

	for (size_t i = 0; i < it->second.size(); ++i) {
		const size_t hook_index = it->second[i];
		if (hook_index >= runtime->hooks.size())
			continue;

		DOSBoxPython_InvokeHook(runtime->hooks[hook_index].python_hook_id);
	}
}

bool MOD_ReadMemoryU8(uint32_t reloc_addr, uint8_t *value)
{
	uint32_t linear_addr = 0;
	if (!value || !translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_readb_checked(linear_addr, value);
}

bool MOD_ReadMemoryU16(uint32_t reloc_addr, uint16_t *value)
{
	uint32_t linear_addr = 0;
	if (!value || !translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_readw_checked(linear_addr, value);
}

bool MOD_ReadMemoryU32(uint32_t reloc_addr, uint32_t *value)
{
	uint32_t linear_addr = 0;
	if (!value || !translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_readd_checked(linear_addr, value);
}

bool MOD_ReadMemoryI32(uint32_t reloc_addr, int32_t *value)
{
	uint32_t raw = 0;
	if (!value || !MOD_ReadMemoryU32(reloc_addr, &raw))
		return false;

	*value = static_cast<int32_t>(raw);
	return true;
}

bool MOD_WriteMemoryU8(uint32_t reloc_addr, uint8_t value)
{
	uint32_t linear_addr = 0;
	if (!translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_writeb_checked(linear_addr, value);
}

bool MOD_WriteMemoryU16(uint32_t reloc_addr, uint16_t value)
{
	uint32_t linear_addr = 0;
	if (!translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_writew_checked(linear_addr, value);
}

bool MOD_WriteMemoryU32(uint32_t reloc_addr, uint32_t value)
{
	uint32_t linear_addr = 0;
	if (!translate_reloc_address(reloc_addr, &linear_addr))
		return false;

	return !mem_writed_checked(linear_addr, value);
}

bool MOD_WriteMemoryI32(uint32_t reloc_addr, int32_t value)
{
	return MOD_WriteMemoryU32(reloc_addr, static_cast<uint32_t>(value));
}
