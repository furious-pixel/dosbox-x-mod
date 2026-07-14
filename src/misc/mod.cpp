#include "mod.h"

#include "control.h"
#include "cpu.h"
#include "dosbox_python.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "timer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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

struct ModNativeCallEventRuntime {
	uint32_t target_reloc = 0;
	uint32_t target_linear = 0;
	uint8_t operation = 0;
};

static const size_t MOD_NATIVE_CALL_EVENT_CAPACITY = 4096u;
static const size_t MOD_NO_NATIVE_CALL_EVENT = static_cast<size_t>(-1);

struct ModCallsiteRuntime {
	std::vector<size_t> python_hook_indices = {};
	size_t native_call_event_index = MOD_NO_NATIVE_CALL_EVENT;
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
	bool process_active = false;
	uint16_t process_psp = 0;
	bool process_start_pending = false;
	uint16_t process_start_psp = 0;
	std::vector<ModHookRuntime> hooks = {};
	std::unordered_map<uint32_t, ModCallsiteRuntime> callsites = {};
	std::vector<ModNativeCallEventRuntime> native_call_event_targets = {};
	std::array<ModNativeCallEvent, MOD_NATIVE_CALL_EVENT_CAPACITY>
	        native_call_event_buffer = {};
	size_t native_call_event_count = 0;
	uint64_t native_call_event_dropped = 0;
};

struct ModRuntime {
	bool initialized = false;
	bool pending_scan = false;
	uint16_t pending_scan_handle = 0;
	size_t pending_scan_index = 0;
	bool active_executable_valid = false;
	size_t active_executable_index = 0;
	bool fast_enabled = false;
	bool unsupported_core_logged = false;
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

static std::string lowercase_ascii_copy(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
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

static bool core_setting_supports_mod_hooks(const std::string &core)
{
	return core == "normal" ||
	       core == "dynamic" ||
	       core == "dynamic_x86" ||
	       core == "dynamic_rec" ||
	       core == "dynamic_nodhfpu";
}

static std::string get_configured_core_setting(const Config& config)
{
	const Section_prop *cpu_section =
	        dynamic_cast<const Section_prop *>(config.GetSection("cpu"));
	if (!cpu_section)
		return {};

	return lowercase_ascii_copy(cpu_section->Get_string("core"));
}

static bool is_supported_non_dynamic_decoder(CPU_Decoder *decoder)
{
	return decoder == &CPU_Core_Normal_Run ||
	       decoder == &CPU_Core_Normal_Trap_Run ||
	       decoder == &CPU_Core286_Normal_Run ||
	       decoder == &CPU_Core286_Normal_Trap_Run ||
	       decoder == &CPU_Core8086_Normal_Run ||
	       decoder == &CPU_Core8086_Normal_Trap_Run ||
	       decoder == &CPU_Core_Prefetch_Run ||
	       decoder == &CPU_Core_Prefetch_Trap_Run ||
	       decoder == &CPU_Core286_Prefetch_Run ||
	       decoder == &CPU_Core8086_Prefetch_Run
#if !defined(C_EMSCRIPTEN)
	       || decoder == &CPU_Core_Simple_Run ||
	       decoder == &CPU_Core_Simple_Trap_Run
#endif
	       ;
}

static const char *describe_active_core(void)
{
#if (C_DYNAMIC_X86)
	if (cpudecoder == &CPU_Core_Dyn_X86_Run || cpudecoder == &CPU_Core_Dyn_X86_Trap_Run)
		return "dynamic";
#endif
#if (C_DYNREC)
	if (cpudecoder == &CPU_Core_Dynrec_Run || cpudecoder == &CPU_Core_Dynrec_Trap_Run)
		return "dynamic";
#endif
	if (cpudecoder == &CPU_Core_Normal_Run || cpudecoder == &CPU_Core_Normal_Trap_Run ||
	    cpudecoder == &CPU_Core286_Normal_Run || cpudecoder == &CPU_Core286_Normal_Trap_Run ||
	    cpudecoder == &CPU_Core8086_Normal_Run || cpudecoder == &CPU_Core8086_Normal_Trap_Run)
		return "normal";
	if (cpudecoder == &CPU_Core_Prefetch_Run || cpudecoder == &CPU_Core_Prefetch_Trap_Run ||
	    cpudecoder == &CPU_Core286_Prefetch_Run || cpudecoder == &CPU_Core8086_Prefetch_Run)
		return "prefetch";
#if !defined(C_EMSCRIPTEN)
	if (cpudecoder == &CPU_Core_Simple_Run || cpudecoder == &CPU_Core_Simple_Trap_Run)
		return "simple";
	if (cpudecoder == &CPU_Core_Full_Run)
		return "full";
#endif
	return "unknown";
}

static bool active_core_supports_mod_hooks(void)
{
	return CPU_IsDynamicCore() != 0 || is_supported_non_dynamic_decoder(cpudecoder);
}

static void maybe_log_unsupported_active_core(void)
{
	if (g_mod.unsupported_core_logged)
		return;
	if (active_core_supports_mod_hooks())
		return;

	g_mod.unsupported_core_logged = true;
	fprintf(stderr,
	        "MOD ERROR: active CPU core '%s' does not support mod hooks; use core=normal or core=dynamic\n",
	        describe_active_core());
	fflush(stderr);
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
	if (!active.process_active)
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

	if (!active_core_supports_mod_hooks()) {
		maybe_log_unsupported_active_core();
		g_mod.fast_enabled = false;
		return;
	}

	g_mod.fast_enabled = runtime->frame_start_ready || !runtime->callsites.empty();
}

static void reset_native_call_events(ModExecutableRuntime &runtime)
{
	runtime.native_call_event_count = 0;
	runtime.native_call_event_dropped = 0;
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
	runtime.callsites.clear();
	runtime.native_call_event_targets.clear();
	reset_native_call_events(runtime);

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
		runtime.callsites[linear_eip].python_hook_indices.push_back(i);
	}

	if (!runtime.config.has_native_call_event_scan_range ||
	    runtime.config.native_call_events.empty()) {
		return;
	}

	for (size_t i = 0; i < runtime.config.native_call_events.size(); ++i) {
		const ModNativeCallEventConfig &config =
		        runtime.config.native_call_events[i];
		uint32_t target_linear = 0;
		if (!apply_delta(config.target_reloc, runtime.delta, &target_linear)) {
			LOG_MSG("MOD ERROR: native call event target 0x%08lX is invalid with delta",
			        static_cast<unsigned long>(config.target_reloc));
			continue;
		}

		ModNativeCallEventRuntime target = {};
		target.target_reloc = config.target_reloc;
		target.target_linear = target_linear;
		target.operation = config.operation;
		runtime.native_call_event_targets.push_back(target);
	}

	uint32_t scan_start = 0;
	uint32_t scan_end = 0;
	if (!apply_delta(runtime.config.native_call_event_scan_start,
	                 runtime.delta,
	                 &scan_start) ||
	    !apply_delta(runtime.config.native_call_event_scan_end,
	                 runtime.delta,
	                 &scan_end) ||
	    scan_start >= scan_end || scan_end - scan_start < 5u) {
		LOG_MSG("MOD ERROR: native call event scan range is invalid with delta");
		return;
	}

	size_t discovered_native_sites = 0;
	for (uint32_t callsite = scan_start; callsite <= scan_end - 5u; ++callsite) {
		uint8_t opcode = 0;
		if (mem_readb_checked(callsite, &opcode) || opcode != 0xe8u)
			continue;

		uint32_t raw_displacement = 0;
		if (mem_readd_checked(callsite + 1u, &raw_displacement))
			continue;

		const int64_t target_value =
		        static_cast<int64_t>(callsite) + 5ll +
		        static_cast<int64_t>(static_cast<int32_t>(raw_displacement));
		if (target_value < 0 || target_value > 0xffffffffll)
			continue;

		const uint32_t target_linear = static_cast<uint32_t>(target_value);
		for (size_t i = 0; i < runtime.native_call_event_targets.size(); ++i) {
			if (runtime.native_call_event_targets[i].target_linear != target_linear)
				continue;
			ModCallsiteRuntime &callsite_runtime = runtime.callsites[callsite];
			if (callsite_runtime.native_call_event_index ==
			    MOD_NO_NATIVE_CALL_EVENT) {
				discovered_native_sites += 1;
			}
			callsite_runtime.native_call_event_index = i;
			break;
		}
	}

	LOG_MSG("MOD: discovered %u native call event site(s)",
	        static_cast<unsigned int>(discovered_native_sites));
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

static void activate_runtime_process(ModExecutableRuntime &runtime,
                                     size_t runtime_index,
                                     uint16_t pspseg)
{
	runtime.process_start_pending = false;
	runtime.process_start_psp = 0;
	runtime.process_active = true;
	runtime.process_psp = pspseg;
	g_mod.active_executable_index = runtime_index;
	g_mod.active_executable_valid = true;
	reset_runtime_frame_state(runtime);
	reset_native_call_events(runtime);
	update_fast_enabled();
	refresh_dynamic_cpu_cache();

	LOG_MSG("MOD: %s active on PSP 0x%04X",
	        runtime.config.name.c_str(),
	        static_cast<unsigned int>(pspseg));
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
			runtime.process_active = false;
			runtime.process_psp = 0;
			update_fast_enabled();
			refresh_dynamic_cpu_cache();
			DOSBoxPython_ResetModStateTiming();
		}
		return;
	}

	const int64_t previous_delta = runtime.delta;
	const bool had_previous_delta = runtime.delta_ready;
	const bool had_pending_start = runtime.process_start_pending;
	const uint16_t pending_start_psp = runtime.process_start_psp;
	const int64_t new_delta = static_cast<int64_t>(found_address) -
	                          static_cast<int64_t>(runtime.config.landmark_reloc);

	runtime.delta = new_delta;
	runtime.delta_ready = true;
	runtime.process_active = false;
	runtime.process_psp = 0;
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

	if (had_pending_start)
		activate_runtime_process(runtime, runtime_index, pending_start_psp);
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

static bool find_executable_runtime_by_name(const char *name, size_t *runtime_index)
{
	if (!name || !runtime_index)
		return false;

	const std::string basename_upper = uppercase_ascii_copy(basename_of(name));
	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		if (basename_upper != g_mod.executables[i].config.name_upper)
			continue;

		*runtime_index = i;
		return true;
	}

	return false;
}

} // namespace

bool MOD_Init(const Config& config)
{
	g_mod = {};

	const std::string core_setting = get_configured_core_setting(config);
	if (!core_setting.empty() && !core_setting_supports_mod_hooks(core_setting)) {
		fprintf(stderr,
		        "MOD ERROR: cpu core setting '%s' is not supported for mod hooks; use core=normal or core=dynamic\n",
		        core_setting.c_str());
		fflush(stderr);
	}

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

	size_t runtime_index = 0;
	if (!find_executable_runtime_by_name(name, &runtime_index))
		return;

	ModExecutableRuntime &runtime = g_mod.executables[runtime_index];
	if (runtime.process_active)
		return;

	g_mod.pending_scan = true;
	g_mod.pending_scan_handle = handle;
	g_mod.pending_scan_index = runtime_index;
	LOG_MSG("MOD: armed %s on handle %u",
	        runtime.config.name.c_str(), static_cast<unsigned int>(handle));
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

void MOD_OnExecutableStarted(const char *name, uint16_t pspseg)
{
	if (!g_mod.initialized || !name || g_mod.executables.empty())
		return;

	size_t runtime_index = 0;
	if (!find_executable_runtime_by_name(name, &runtime_index))
		return;

	ModExecutableRuntime &runtime = g_mod.executables[runtime_index];
	if (!runtime.delta_ready) {
		runtime.process_start_pending = true;
		runtime.process_start_psp = pspseg;
		g_mod.active_executable_index = runtime_index;
		g_mod.active_executable_valid = true;
		LOG_MSG("MOD: %s start pending landmark scan on PSP 0x%04X",
		        runtime.config.name.c_str(),
		        static_cast<unsigned int>(pspseg));
		return;
	}

	activate_runtime_process(runtime, runtime_index, pspseg);
}

void MOD_OnTerminatePSP(uint16_t pspseg, bool tsr, uint8_t exitcode)
{
	if (!g_mod.initialized)
		return;

	if (g_mod.active_executable_valid &&
	    g_mod.active_executable_index < g_mod.executables.size()) {
		ModExecutableRuntime &runtime =
		        g_mod.executables[g_mod.active_executable_index];
		if (runtime.process_active && runtime.process_psp == pspseg) {
			runtime.process_active = false;
			runtime.process_psp = 0;
			runtime.process_start_pending = false;
			runtime.process_start_psp = 0;
			g_mod.active_executable_valid = false;
			reset_runtime_frame_state(runtime);
			reset_native_call_events(runtime);
			update_fast_enabled();
			refresh_dynamic_cpu_cache();
			DOSBoxPython_ResetModRuntimeState();

			LOG_MSG("MOD: %s exited PSP 0x%04X with code %u%s",
			        runtime.config.name.c_str(),
			        static_cast<unsigned int>(pspseg),
			        static_cast<unsigned int>(exitcode),
			        tsr ? " as TSR" : "");
			return;
		}
	}

	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		ModExecutableRuntime &runtime = g_mod.executables[i];
		if (!runtime.process_start_pending ||
		    runtime.process_start_psp != pspseg) {
			continue;
		}

		runtime.process_start_pending = false;
		runtime.process_start_psp = 0;
		if (g_mod.active_executable_valid &&
		    g_mod.active_executable_index == i) {
			g_mod.active_executable_valid = false;
			update_fast_enabled();
			refresh_dynamic_cpu_cache();
		}

		LOG_MSG("MOD: %s exited PSP 0x%04X before landmark scan completed",
		        runtime.config.name.c_str(),
		        static_cast<unsigned int>(pspseg));
		return;
	}
}

bool MOD_FastEnabled(void)
{
	return g_mod.fast_enabled;
}

bool MOD_RenderActive(void)
{
	ModExecutableRuntime *runtime = NULL;
	return get_active_runtime(&runtime);
}

void MOD_OnCallsite(uint32_t linear_eip)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return;

	if (runtime->frame_start_ready && linear_eip == runtime->frame_start_linear)
		update_frame_timing(*runtime);

	const std::unordered_map<uint32_t, ModCallsiteRuntime>::const_iterator it =
	        runtime->callsites.find(linear_eip);
	if (it == runtime->callsites.end())
		return;

	const size_t native_event_index = it->second.native_call_event_index;
	if (native_event_index < runtime->native_call_event_targets.size()) {
		if (runtime->native_call_event_count < MOD_NATIVE_CALL_EVENT_CAPACITY) {
			const ModNativeCallEventRuntime &target =
			        runtime->native_call_event_targets[native_event_index];
			ModNativeCallEvent &event =
			        runtime->native_call_event_buffer[runtime->native_call_event_count++];
			event.value = reg_eax;
			event.source_reloc = static_cast<uint32_t>(
			        static_cast<int64_t>(linear_eip) - runtime->delta);
			event.operation = target.operation;
		} else {
			runtime->native_call_event_dropped += 1;
		}
	}

	for (size_t i = 0; i < it->second.python_hook_indices.size(); ++i) {
		const size_t hook_index = it->second.python_hook_indices[i];
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

bool MOD_ReadMemoryBlock(uint32_t reloc_addr, uint8_t *data, size_t size)
{
	uint32_t linear_addr = 0;
	if ((!data && size != 0) || !translate_reloc_address(reloc_addr, &linear_addr))
		return false;
	if (size == 0)
		return true;
	if (static_cast<uint64_t>(linear_addr) + static_cast<uint64_t>(size) - 1u >
	    0xffffffffull)
		return false;

	MEM_BlockRead(linear_addr, data, static_cast<Bitu>(size));
	return true;
}

bool MOD_GetActiveDelta(int64_t *delta)
{
	ModExecutableRuntime *runtime = NULL;
	if (!delta || !get_active_runtime(&runtime))
		return false;

	*delta = runtime->delta;
	return true;
}

bool MOD_ReadRuntimeMemoryU8(uint32_t linear_addr, uint8_t *value)
{
	if (!value)
		return false;

	return !mem_readb_checked(linear_addr, value);
}

bool MOD_ReadRuntimeMemoryU16(uint32_t linear_addr, uint16_t *value)
{
	if (!value)
		return false;

	return !mem_readw_checked(linear_addr, value);
}

bool MOD_ReadRuntimeMemoryU32(uint32_t linear_addr, uint32_t *value)
{
	if (!value)
		return false;

	return !mem_readd_checked(linear_addr, value);
}

bool MOD_ReadRuntimeMemoryI32(uint32_t linear_addr, int32_t *value)
{
	uint32_t raw = 0;
	if (!value || !MOD_ReadRuntimeMemoryU32(linear_addr, &raw))
		return false;

	*value = static_cast<int32_t>(raw);
	return true;
}

bool MOD_ReadRuntimeMemoryBlock(uint32_t linear_addr, uint8_t *data, size_t size)
{
	if (!data && size != 0)
		return false;
	if (size == 0)
		return true;
	if (static_cast<uint64_t>(linear_addr) + static_cast<uint64_t>(size) - 1u >
	    0xffffffffull)
		return false;

	MEM_BlockRead(linear_addr, data, static_cast<Bitu>(size));
	return true;
}

bool MOD_DrainNativeCallEvents(std::vector<ModNativeCallEvent> *events,
                               uint64_t *dropped)
{
	if (!events || !dropped)
		return false;

	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return false;

	events->assign(runtime->native_call_event_buffer.begin(),
	               runtime->native_call_event_buffer.begin() +
	                       runtime->native_call_event_count);
	*dropped = runtime->native_call_event_dropped;
	reset_native_call_events(*runtime);
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
