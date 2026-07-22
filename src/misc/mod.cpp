#include "mod.h"

#include "callback.h"
#include "control.h"
#include "cpu.h"
#include "dosbox.h"
#include "dosbox_python.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "timer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

Bitu FillFlags(void);
void DestroyConditionFlags(void);
void CALLBACK_DeAllocate(Bitu callback);

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

struct ModCallSuppressionSiteRuntime {
	uint32_t callsite_reloc = 0;
	uint32_t callsite_linear = 0;
	uint32_t target_reloc = 0;
	int32_t call_displacement = 0;
	uint64_t executed = 0;
	uint64_t skipped = 0;
};

static const size_t MOD_NATIVE_CALL_EVENT_CAPACITY = 4096u;
static const size_t MOD_NO_NATIVE_CALL_EVENT = static_cast<size_t>(-1);
static const size_t MOD_NO_CALL_SUPPRESSION_SITE = static_cast<size_t>(-1);

struct ModCallsiteRuntime {
	std::vector<size_t> python_hook_indices = {};
	size_t native_call_event_index = MOD_NO_NATIVE_CALL_EVENT;
	bool scene_raster_phase_enter = false;
	bool scene_raster_phase_leave = false;
	size_t scene_raster_suppression_index = MOD_NO_CALL_SUPPRESSION_SITE;
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
	bool scene_raster_suppression_validated = false;
	bool scene_raster_suppression_requested = false;
	bool scene_raster_phase_active = false;
	bool scene_raster_callsite_hooks_active = false;
	uint32_t scene_raster_phase_enter_linear = 0;
	uint32_t scene_raster_phase_leave_linear = 0;
	uint64_t scene_raster_request_frame = 0;
	std::vector<ModCallSuppressionSiteRuntime>
	        scene_raster_suppression_sites = {};
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
	bool dynamic_cache_refresh_pending = false;
	bool safe_point_pending = false;
	bool safe_point_active = false;
	bool guest_call_active = false;
	bool guest_call_stopped = false;
	Bitu guest_call_stop_callback = 0;
	std::vector<ModExecutableRuntime> executables = {};
};

ModRuntime g_mod = {};

static const size_t MOD_TIMING_HISTORY_CAPACITY = 256u;
static const uint64_t MOD_TIMING_WARMUP_READY_FRAMES = 180u;
static const uint64_t MOD_TIMING_SUMMARY_INTERVAL_FRAMES = 600u;
static const double MOD_TIMING_SPIKE_THRESHOLD_MS = 25.0;

struct ModTimingAccumulator {
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> elapsed_ns = {};
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> maximum_ns = {};
	std::array<uint32_t, MOD_TIMING_CATEGORY_COUNT> calls = {};
	uint64_t decoder_cycles = 0;
	uint64_t decoder_max_slice_cycles = 0;
	uint64_t decoder_slowest_elapsed_ns = 0;
	uint64_t decoder_slowest_cycles = 0;
	int64_t decoder_slowest_requested_cycles = 0;
	int64_t decoder_slowest_cycle_max = 0;
	bool decoder_slowest_auto_adjust = false;
	uint32_t auto_cycle_adjustments = 0;
	int64_t auto_cycle_max_before = 0;
	int64_t auto_cycle_max_after = 0;
	int32_t auto_ticks_added = 0;
	int32_t auto_ticks_scheduled = 0;
	int32_t auto_ticks_done = 0;
	uint32_t native_frames = 0;
	uint32_t ready_frames = 0;
	uint32_t presentations = 0;
	uint32_t new_mod_presentations = 0;
};

struct ModTimingHistory {
	std::array<double, MOD_TIMING_HISTORY_CAPACITY> values = {};
	size_t count = 0;
	size_t next = 0;
};

struct ModTimingState {
	uint64_t frame_boundary_ns = 0;
	uint64_t present_boundary_ns = 0;
	uint64_t ready_total = 0;
	uint64_t last_frame = 0;
	uint64_t frame_spikes = 0;
	uint64_t present_spikes = 0;
	bool telemetry_log_previous_interval = false;
	ModTimingAccumulator current = {};
	ModTimingHistory frame_history = {};
	ModTimingHistory present_history = {};
	ModTimingSummary summary = {};
};

static ModTimingState g_mod_timing = {};
static FILE *g_mod_timing_log = NULL;
// Keep ordinary sessions in memory so spike reporting never forces disk I/O.
static std::array<char, 1024u * 1024u> g_mod_timing_log_buffer = {};

static uint64_t timing_now_ns(void)
{
	const std::chrono::steady_clock::time_point now =
	        std::chrono::steady_clock::now();
	return static_cast<uint64_t>(
	        std::chrono::duration_cast<std::chrono::nanoseconds>(
	                now.time_since_epoch()).count());
}

static double timing_ns_to_ms(const uint64_t value)
{
	return static_cast<double>(value) / 1000000.0;
}

static void timing_history_add(ModTimingHistory &history, const double value)
{
	history.values[history.next] = value;
	history.next = (history.next + 1u) % history.values.size();
	history.count = std::min(history.count + 1u, history.values.size());
}

static double timing_history_percentile(const ModTimingHistory &history,
	                                    const double percentile)
{
	if (history.count == 0)
		return 0.0;

	std::array<double, MOD_TIMING_HISTORY_CAPACITY> sorted = {};
	std::copy(history.values.begin(),
	          history.values.begin() + history.count,
	          sorted.begin());
	std::sort(sorted.begin(), sorted.begin() + history.count);
	const size_t index = static_cast<size_t>(std::ceil(
	        percentile * static_cast<double>(history.count - 1u)));
	return sorted[std::min(index, history.count - 1u)];
}

static void refresh_timing_summary(void)
{
	ModTimingSummary &summary = g_mod_timing.summary;
	summary.valid = g_mod_timing.frame_history.count >= 60u;
	summary.frame_p50_ms = timing_history_percentile(
	        g_mod_timing.frame_history, 0.50);
	summary.frame_p95_ms = timing_history_percentile(
	        g_mod_timing.frame_history, 0.95);
	summary.frame_p99_ms = timing_history_percentile(
	        g_mod_timing.frame_history, 0.99);
	summary.frame_max_ms = timing_history_percentile(
	        g_mod_timing.frame_history, 1.00);
	summary.present_p50_ms = timing_history_percentile(
	        g_mod_timing.present_history, 0.50);
	summary.present_p95_ms = timing_history_percentile(
	        g_mod_timing.present_history, 0.95);
	summary.present_p99_ms = timing_history_percentile(
	        g_mod_timing.present_history, 0.99);
	summary.present_max_ms = timing_history_percentile(
	        g_mod_timing.present_history, 1.00);
	summary.frame_spikes = g_mod_timing.frame_spikes;
	summary.present_spikes = g_mod_timing.present_spikes;
}

static bool timing_log_line(const char *format, ...)
{
	if (!g_mod_timing_log) {
		g_mod_timing_log = fopen("frame_timing.log", "a");
		if (!g_mod_timing_log)
			return false;
		setvbuf(g_mod_timing_log,
		        g_mod_timing_log_buffer.data(),
		        _IOFBF,
		        g_mod_timing_log_buffer.size());
	}

	va_list arguments;
	va_start(arguments, format);
	vfprintf(g_mod_timing_log, format, arguments);
	va_end(arguments);
	fputc('\n', g_mod_timing_log);
	return true;
}

static void reset_mod_timing_state(void)
{
	g_mod_timing = {};
}

static void close_mod_timing_log(void)
{
	if (!g_mod_timing_log)
		return;
	fflush(g_mod_timing_log);
	fclose(g_mod_timing_log);
	g_mod_timing_log = NULL;
}

static void format_recent_timing_history(const ModTimingHistory &history,
	                                     char *output,
	                                     const size_t output_size)
{
	if (!output || output_size == 0)
		return;
	output[0] = 0;
	const size_t wanted = std::min<size_t>(8u, history.count);
	const size_t start = (history.next + history.values.size() - wanted) %
	                     history.values.size();
	size_t used = 0;
	for (size_t i = 0; i < wanted && used < output_size; ++i) {
		const size_t index = (start + i) % history.values.size();
		const int written = snprintf(output + used,
		                             output_size - used,
		                             "%s%.2f",
		                             i == 0 ? "" : ",",
		                             history.values[index]);
		if (written <= 0)
			break;
		used += std::min<size_t>(static_cast<size_t>(written),
		                         output_size - used);
	}
}

static void timing_frame_boundary(const uint64_t frame)
{
	const uint64_t now = timing_now_ns();
	if (g_mod_timing.frame_boundary_ns == 0) {
		g_mod_timing.frame_boundary_ns = now;
		g_mod_timing.last_frame = frame;
		g_mod_timing.current = {};
		return;
	}

	const double frame_ms = timing_ns_to_ms(
	        now - g_mod_timing.frame_boundary_ns);
	g_mod_timing.frame_boundary_ns = now;
	g_mod_timing.last_frame = frame;
	const ModTimingAccumulator sample = g_mod_timing.current;
	g_mod_timing.current = {};

	const bool steady_state =
	        g_mod_timing.ready_total > MOD_TIMING_WARMUP_READY_FRAMES &&
	        sample.ready_frames != 0;
	bool wrote_log = false;
	if (steady_state) {
		timing_history_add(g_mod_timing.frame_history, frame_ms);
		if (frame_ms >= MOD_TIMING_SPIKE_THRESHOLD_MS) {
			g_mod_timing.frame_spikes++;
			char recent[160] = {};
			format_recent_timing_history(
			        g_mod_timing.frame_history, recent, sizeof(recent));
			const double cpu_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_CPU_DECODER]);
			const double pic_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_PIC_EVENT]);
			const double safe_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_SAFE_POINT]);
			const double events_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_GFX_EVENTS]);
			const double timer_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_TIMER_TICK]);
			const double tick_ms = timing_ns_to_ms(
			        sample.elapsed_ns[MOD_TIMING_TICK_CONTROL]);
			const double other_ms = std::max(
			        0.0,
			        frame_ms - cpu_ms - pic_ms - safe_ms - events_ms -
			                timer_ms - tick_ms);
			wrote_log = timing_log_line(
			        "FRAME_TIMING spike frame=%llu frame_ms=%.3f cpu_ms=%.3f cpu_max_ms=%.3f cpu_calls=%u cpu_cycles=%llu cpu_cycles_max=%llu cpu_slowest_cycles=%llu cpu_slowest_request=%lld cpu_slowest_cmax=%lld cpu_slowest_auto=%u pic_ms=%.3f pic_max_ms=%.3f pic_calls=%u hook_ms=%.3f hook_max_ms=%.3f hook_calls=%u safe_ms=%.3f events_ms=%.3f events_max_ms=%.3f events_calls=%u timer_ms=%.3f timer_max_ms=%.3f timer_calls=%u tick_ms=%.3f tick_max_ms=%.3f tick_calls=%u sleep_ms=%.3f sleep_max_ms=%.3f sleep_calls=%u auto_ms=%.3f auto_max_ms=%.3f auto_calls=%u auto_adjustments=%u auto_cmax_before=%lld auto_cmax_after=%lld auto_ticks_added=%d auto_ticks_scheduled=%d auto_ticks_done=%d dyn_ms=%.3f dyn_max_ms=%.3f dyn_blocks=%u compositor_ms=%.3f swap_ms=%.3f other_ms=%.3f native_frames=%u ready_frames=%u presentations=%u new_mod_presentations=%u prior_telemetry_log=%u recent_frame_ms=%s",
			        static_cast<unsigned long long>(frame > 0 ? frame - 1u : 0u),
			        frame_ms,
			        cpu_ms,
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_CPU_DECODER]),
			        sample.calls[MOD_TIMING_CPU_DECODER],
			        static_cast<unsigned long long>(sample.decoder_cycles),
			        static_cast<unsigned long long>(sample.decoder_max_slice_cycles),
			        static_cast<unsigned long long>(sample.decoder_slowest_cycles),
			        static_cast<long long>(sample.decoder_slowest_requested_cycles),
			        static_cast<long long>(sample.decoder_slowest_cycle_max),
			        sample.decoder_slowest_auto_adjust ? 1u : 0u,
			        pic_ms,
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_PIC_EVENT]),
			        sample.calls[MOD_TIMING_PIC_EVENT],
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_PYTHON_HOOK]),
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_PYTHON_HOOK]),
			        sample.calls[MOD_TIMING_PYTHON_HOOK],
			        safe_ms,
			        events_ms,
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_GFX_EVENTS]),
			        sample.calls[MOD_TIMING_GFX_EVENTS],
			        timer_ms,
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_TIMER_TICK]),
			        sample.calls[MOD_TIMING_TIMER_TICK],
			        tick_ms,
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_TICK_CONTROL]),
			        sample.calls[MOD_TIMING_TICK_CONTROL],
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_TICK_SLEEP]),
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_TICK_SLEEP]),
			        sample.calls[MOD_TIMING_TICK_SLEEP],
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_AUTO_CYCLE]),
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_AUTO_CYCLE]),
			        sample.calls[MOD_TIMING_AUTO_CYCLE],
			        sample.auto_cycle_adjustments,
			        static_cast<long long>(sample.auto_cycle_max_before),
			        static_cast<long long>(sample.auto_cycle_max_after),
			        sample.auto_ticks_added,
			        sample.auto_ticks_scheduled,
			        sample.auto_ticks_done,
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_DYNAMIC_COMPILE]),
			        timing_ns_to_ms(sample.maximum_ns[MOD_TIMING_DYNAMIC_COMPILE]),
			        sample.calls[MOD_TIMING_DYNAMIC_COMPILE],
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_COMPOSITOR]),
			        timing_ns_to_ms(sample.elapsed_ns[MOD_TIMING_SWAP]),
			        other_ms,
			        sample.native_frames,
			        sample.ready_frames,
			        sample.presentations,
			        sample.new_mod_presentations,
			        g_mod_timing.telemetry_log_previous_interval ? 1u : 0u,
			        recent);
		}

		if (frame % MOD_TIMING_SUMMARY_INTERVAL_FRAMES == 0u) {
			refresh_timing_summary();
			wrote_log = timing_log_line(
			        "FRAME_TIMING summary frame=%llu ready_warmup=%llu samples=%u frame_ms_p50=%.3f frame_ms_p95=%.3f frame_ms_p99=%.3f frame_ms_max=%.3f present_ms_p50=%.3f present_ms_p95=%.3f present_ms_p99=%.3f present_ms_max=%.3f frame_spikes=%llu present_spikes=%llu",
			        static_cast<unsigned long long>(frame),
			        static_cast<unsigned long long>(MOD_TIMING_WARMUP_READY_FRAMES),
			        static_cast<unsigned int>(g_mod_timing.frame_history.count),
			        g_mod_timing.summary.frame_p50_ms,
			        g_mod_timing.summary.frame_p95_ms,
			        g_mod_timing.summary.frame_p99_ms,
			        g_mod_timing.summary.frame_max_ms,
			        g_mod_timing.summary.present_p50_ms,
			        g_mod_timing.summary.present_p95_ms,
			        g_mod_timing.summary.present_p99_ms,
			        g_mod_timing.summary.present_max_ms,
			        static_cast<unsigned long long>(g_mod_timing.frame_spikes),
			        static_cast<unsigned long long>(g_mod_timing.present_spikes)) ||
			        wrote_log;
		} else if (g_mod_timing.frame_history.count % 120u == 0u) {
			refresh_timing_summary();
		}
	}

	g_mod_timing.telemetry_log_previous_interval = wrote_log;
}

static Bitu guest_call_stop_handler(void)
{
	if (!g_mod.guest_call_active)
		return CBRET_NONE;

	g_mod.guest_call_stopped = true;
	return CBRET_STOP;
}

static bool ensure_guest_call_stop_callback(void)
{
	if (g_mod.guest_call_stop_callback != 0)
		return true;

	const Bitu callback = CALLBACK_Allocate();
	if (!CALLBACK_Setup(callback,
	                    guest_call_stop_handler,
	                    CB_RETN,
	                    "mod guest call stop")) {
		CALLBACK_DeAllocate(callback);
		return false;
	}

	g_mod.guest_call_stop_callback = callback;
	return true;
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

static void reset_scene_raster_suppression(ModExecutableRuntime &runtime,
	                                         bool reset_counters)
{
	runtime.scene_raster_suppression_requested = false;
	runtime.scene_raster_phase_active = false;
	runtime.scene_raster_request_frame = 0;
	if (!reset_counters)
		return;

	for (size_t i = 0; i < runtime.scene_raster_suppression_sites.size(); ++i) {
		runtime.scene_raster_suppression_sites[i].executed = 0;
		runtime.scene_raster_suppression_sites[i].skipped = 0;
	}
}

static void prune_empty_callsite(ModExecutableRuntime &runtime,
	                              uint32_t linear_eip)
{
	std::unordered_map<uint32_t, ModCallsiteRuntime>::iterator it =
	        runtime.callsites.find(linear_eip);
	if (it == runtime.callsites.end())
		return;

	const ModCallsiteRuntime &callsite = it->second;
	if (callsite.python_hook_indices.empty() &&
	    callsite.native_call_event_index == MOD_NO_NATIVE_CALL_EVENT &&
	    !callsite.scene_raster_phase_enter &&
	    !callsite.scene_raster_phase_leave &&
	    callsite.scene_raster_suppression_index ==
	            MOD_NO_CALL_SUPPRESSION_SITE) {
		runtime.callsites.erase(it);
	}
}

static void set_scene_raster_callsite_hooks(ModExecutableRuntime &runtime,
	                                         bool enabled)
{
	enabled = enabled && runtime.scene_raster_suppression_validated;
	if (runtime.scene_raster_callsite_hooks_active == enabled)
		return;

	if (enabled) {
		runtime.callsites[runtime.scene_raster_phase_enter_linear]
		        .scene_raster_phase_enter = true;
		runtime.callsites[runtime.scene_raster_phase_leave_linear]
		        .scene_raster_phase_leave = true;
		for (size_t i = 0; i < runtime.scene_raster_suppression_sites.size(); ++i) {
			const uint32_t linear_eip =
			        runtime.scene_raster_suppression_sites[i].callsite_linear;
			runtime.callsites[linear_eip].scene_raster_suppression_index = i;
		}
	} else {
		std::unordered_map<uint32_t, ModCallsiteRuntime>::iterator it =
		        runtime.callsites.find(runtime.scene_raster_phase_enter_linear);
		if (it != runtime.callsites.end())
			it->second.scene_raster_phase_enter = false;
		prune_empty_callsite(runtime, runtime.scene_raster_phase_enter_linear);

		it = runtime.callsites.find(runtime.scene_raster_phase_leave_linear);
		if (it != runtime.callsites.end())
			it->second.scene_raster_phase_leave = false;
		prune_empty_callsite(runtime, runtime.scene_raster_phase_leave_linear);

		for (size_t i = 0; i < runtime.scene_raster_suppression_sites.size(); ++i) {
			const uint32_t linear_eip =
			        runtime.scene_raster_suppression_sites[i].callsite_linear;
			it = runtime.callsites.find(linear_eip);
			if (it != runtime.callsites.end()) {
				it->second.scene_raster_suppression_index =
				        MOD_NO_CALL_SUPPRESSION_SITE;
			}
			prune_empty_callsite(runtime, linear_eip);
		}
		runtime.scene_raster_phase_active = false;
	}

	runtime.scene_raster_callsite_hooks_active = enabled;
}

static void sync_scene_raster_callsite_hooks(void)
{
	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		ModExecutableRuntime &runtime = g_mod.executables[i];
		set_scene_raster_callsite_hooks(
		        runtime,
		        runtime.scene_raster_suppression_requested);
	}
}

static void reset_runtime_frame_state(ModExecutableRuntime &runtime)
{
	runtime.frame_started = false;
	runtime.frame_counter_start = 0;
	runtime.frame_counter_last = 0;
	runtime.frame_state = {};
	reset_mod_timing_state();
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
	runtime.scene_raster_suppression_sites.clear();
	runtime.scene_raster_suppression_validated = false;
	runtime.scene_raster_callsite_hooks_active = false;
	runtime.scene_raster_phase_enter_linear = 0;
	runtime.scene_raster_phase_leave_linear = 0;
	reset_native_call_events(runtime);
	reset_scene_raster_suppression(runtime, true);

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

	if (runtime.config.has_scene_raster_suppression &&
	    !runtime.config.scene_raster_suppression_sites.empty()) {
		uint32_t phase_enter_linear = 0;
		uint32_t phase_leave_linear = 0;
		bool valid = apply_delta(
		                     runtime.config.scene_raster_phase_enter_reloc,
		                     runtime.delta,
		                     &phase_enter_linear) &&
		             apply_delta(
		                     runtime.config.scene_raster_phase_leave_reloc,
		                     runtime.delta,
		                     &phase_leave_linear);
		if (!valid) {
			LOG_MSG("MOD ERROR: %s scene raster phase boundaries are invalid with delta",
			        runtime.config.name.c_str());
		} else {
			runtime.scene_raster_phase_enter_linear = phase_enter_linear;
			runtime.scene_raster_phase_leave_linear = phase_leave_linear;
		}

		for (size_t i = 0;
		     valid && i < runtime.config.scene_raster_suppression_sites.size();
		     ++i) {
			const ModCallSuppressionSiteConfig &config =
			        runtime.config.scene_raster_suppression_sites[i];
			uint32_t callsite_linear = 0;
			uint32_t target_linear = 0;
			uint8_t opcode = 0;
			uint32_t raw_displacement = 0;
			if (!apply_delta(config.callsite_reloc,
			                 runtime.delta,
			                 &callsite_linear) ||
			    !apply_delta(config.target_reloc,
			                 runtime.delta,
			                 &target_linear) ||
			    mem_readb_checked(callsite_linear, &opcode) ||
			    opcode != 0xe8u ||
			    mem_readd_checked(callsite_linear + 1u, &raw_displacement)) {
				valid = false;
				LOG_MSG("MOD ERROR: scene raster site 0x%08lX is not a readable near call",
				        static_cast<unsigned long>(config.callsite_reloc));
				break;
			}

			const int32_t displacement =
			        static_cast<int32_t>(raw_displacement);
			const int64_t decoded_target =
			        static_cast<int64_t>(callsite_linear) + 5ll +
			        static_cast<int64_t>(displacement);
			if (decoded_target != static_cast<int64_t>(target_linear)) {
				valid = false;
				LOG_MSG("MOD ERROR: scene raster site 0x%08lX target signature mismatch",
				        static_cast<unsigned long>(config.callsite_reloc));
				break;
			}

			ModCallSuppressionSiteRuntime site = {};
			site.callsite_reloc = config.callsite_reloc;
			site.callsite_linear = callsite_linear;
			site.target_reloc = config.target_reloc;
			site.call_displacement = displacement;
			runtime.scene_raster_suppression_sites.push_back(site);
		}

		runtime.scene_raster_suppression_validated =
		        valid &&
		        runtime.scene_raster_suppression_sites.size() ==
		                runtime.config.scene_raster_suppression_sites.size();
		if (runtime.scene_raster_suppression_validated) {
			LOG_MSG("MOD: validated %u phase-scoped scene raster suppression site(s)",
			        static_cast<unsigned int>(
			                runtime.scene_raster_suppression_sites.size()));
		} else {
			reset_scene_raster_suppression(runtime, true);
		}
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
	set_scene_raster_callsite_hooks(runtime, false);
	reset_scene_raster_suppression(runtime, true);
	update_fast_enabled();
	refresh_dynamic_cpu_cache();
	timing_log_line(
	        "FRAME_TIMING session executable=%s warmup_ready_frames=%llu spike_threshold_ms=%.3f history_capacity=%u",
	        runtime.config.name.c_str(),
	        static_cast<unsigned long long>(MOD_TIMING_WARMUP_READY_FRAMES),
	        MOD_TIMING_SPIKE_THRESHOLD_MS,
	        static_cast<unsigned int>(MOD_TIMING_HISTORY_CAPACITY));

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
			set_scene_raster_callsite_hooks(runtime, false);
			reset_scene_raster_suppression(runtime, false);
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

	timing_frame_boundary(runtime.frame_state.frame);
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

	timing_frame_boundary(runtime.frame_state.frame);
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

uint64_t MOD_TimingBegin(void)
{
	if (!g_mod.fast_enabled)
		return 0;
	return timing_now_ns();
}

uint64_t MOD_TimingEnd(const ModTimingCategory category,
	                   const uint64_t started_ns)
{
	if (started_ns == 0 || category < 0 ||
	    category >= MOD_TIMING_CATEGORY_COUNT) {
		return 0;
	}

	const uint64_t now = timing_now_ns();
	const uint64_t elapsed = now >= started_ns ? now - started_ns : 0;
	ModTimingAccumulator &current = g_mod_timing.current;
	current.elapsed_ns[category] += elapsed;
	current.maximum_ns[category] =
	        std::max(current.maximum_ns[category], elapsed);
	current.calls[category]++;
	return elapsed;
}

void MOD_TimingRecordDecoderSlice(const uint64_t elapsed_ns,
	                              const int64_t requested_cycles,
	                              const int64_t remaining_cycles,
	                              const int64_t cycle_max,
	                              const bool auto_adjust)
{
	if (elapsed_ns == 0)
		return;

	ModTimingAccumulator &current = g_mod_timing.current;
	const int64_t executed_signed = requested_cycles - remaining_cycles;
	const uint64_t executed_cycles = executed_signed > 0
	                                       ? static_cast<uint64_t>(executed_signed)
	                                       : 0u;
	current.decoder_cycles += executed_cycles;
	current.decoder_max_slice_cycles = std::max(
	        current.decoder_max_slice_cycles, executed_cycles);
	if (elapsed_ns >= current.decoder_slowest_elapsed_ns) {
		current.decoder_slowest_elapsed_ns = elapsed_ns;
		current.decoder_slowest_cycles = executed_cycles;
		current.decoder_slowest_requested_cycles = requested_cycles;
		current.decoder_slowest_cycle_max = cycle_max;
		current.decoder_slowest_auto_adjust = auto_adjust;
	}
}

void MOD_TimingRecordAutoCycleAdjustment(const int64_t cycle_max_before,
	                                     const int64_t cycle_max_after,
	                                     const int32_t ticks_added,
	                                     const int32_t ticks_scheduled,
	                                     const int32_t ticks_done)
{
	if (!g_mod.fast_enabled)
		return;

	ModTimingAccumulator &current = g_mod_timing.current;
	current.auto_cycle_adjustments++;
	current.auto_cycle_max_before = cycle_max_before;
	current.auto_cycle_max_after = cycle_max_after;
	current.auto_ticks_added = ticks_added;
	current.auto_ticks_scheduled = ticks_scheduled;
	current.auto_ticks_done = ticks_done;
}

void MOD_TimingCountNativeFrame(void)
{
	if (g_mod.fast_enabled)
		g_mod_timing.current.native_frames++;
}

void MOD_TimingCountModFrameReady(void)
{
	if (!g_mod.fast_enabled)
		return;
	g_mod_timing.current.ready_frames++;
	g_mod_timing.ready_total++;
}

void MOD_TimingPresentationBoundary(const bool compositor_invoked,
	                                const bool new_mod_frame,
	                                const uint64_t compositor_ns,
	                                const uint64_t swap_ns,
	                                const char *source)
{
	if (!g_mod.fast_enabled)
		return;

	ModTimingState &timing = g_mod_timing;
	timing.current.presentations++;
	if (new_mod_frame)
		timing.current.new_mod_presentations++;

	const uint64_t now = timing_now_ns();
	if (timing.present_boundary_ns == 0) {
		timing.present_boundary_ns = now;
		return;
	}

	const double present_ms = timing_ns_to_ms(
	        now - timing.present_boundary_ns);
	timing.present_boundary_ns = now;
	if (!new_mod_frame ||
	    timing.ready_total <= MOD_TIMING_WARMUP_READY_FRAMES) {
		return;
	}

	timing_history_add(timing.present_history, present_ms);
	if (present_ms < MOD_TIMING_SPIKE_THRESHOLD_MS)
		return;

	timing.present_spikes++;
	char recent[160] = {};
	format_recent_timing_history(
	        timing.present_history, recent, sizeof(recent));
	if (timing_log_line(
	        "PRESENT_TIMING spike frame=%llu source=%s present_ms=%.3f compositor_invoked=%u compositor_ms=%.3f swap_ms=%.3f recent_present_ms=%s",
	        static_cast<unsigned long long>(timing.last_frame),
	        source ? source : "unknown",
	        present_ms,
	        compositor_invoked ? 1u : 0u,
	        timing_ns_to_ms(compositor_ns),
	        timing_ns_to_ms(swap_ns),
	        recent)) {
		timing.telemetry_log_previous_interval = true;
	}
}

bool MOD_GetTimingSummary(ModTimingSummary *summary)
{
	if (!summary)
		return false;
	*summary = g_mod_timing.summary;
	return summary->valid;
}

bool MOD_Init(const Config& config)
{
	g_mod = {};
	reset_mod_timing_state();

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
	if (g_mod.guest_call_stop_callback != 0)
		CALLBACK_DeAllocate(g_mod.guest_call_stop_callback);
	g_mod = {};
	DOSBoxPython_Shutdown();
	close_mod_timing_log();
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
			g_mod.safe_point_pending = false;
			runtime.process_active = false;
			runtime.process_psp = 0;
			runtime.process_start_pending = false;
			runtime.process_start_psp = 0;
			g_mod.active_executable_valid = false;
			reset_runtime_frame_state(runtime);
			reset_native_call_events(runtime);
			set_scene_raster_callsite_hooks(runtime, false);
			reset_scene_raster_suppression(runtime, true);
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

bool MOD_GuestCallActive(void)
{
	return g_mod.guest_call_active;
}

bool MOD_SetSceneRasterSuppression(bool enabled)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return false;

	if (!enabled) {
		const bool changed = runtime->scene_raster_suppression_requested;
		reset_scene_raster_suppression(*runtime, false);
		if (changed)
			g_mod.dynamic_cache_refresh_pending = true;
		return true;
	}

	if (!runtime->scene_raster_suppression_validated)
		return false;

	const bool changed = !runtime->scene_raster_suppression_requested;
	runtime->scene_raster_suppression_requested = true;
	runtime->scene_raster_request_frame = runtime->frame_state.frame;
	if (changed)
		g_mod.dynamic_cache_refresh_pending = true;
	return true;
}

bool MOD_CallsiteCanBeSuppressed(uint32_t linear_eip)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime) ||
	    !runtime->scene_raster_suppression_validated) {
		return false;
	}

	const std::unordered_map<uint32_t, ModCallsiteRuntime>::const_iterator it =
	        runtime->callsites.find(linear_eip);
	return runtime->scene_raster_suppression_requested &&
	       it != runtime->callsites.end() &&
	       it->second.scene_raster_suppression_index <
	               runtime->scene_raster_suppression_sites.size();
}

void MOD_DisableSceneRasterSuppression(void)
{
	bool changed = false;
	for (size_t i = 0; i < g_mod.executables.size(); ++i) {
		changed = changed ||
		          g_mod.executables[i].scene_raster_suppression_requested;
		reset_scene_raster_suppression(g_mod.executables[i], false);
	}
	if (changed)
		g_mod.dynamic_cache_refresh_pending = true;
}

bool MOD_GetSceneRasterSuppressionStats(ModSceneRasterSuppressionStats *stats)
{
	ModExecutableRuntime *runtime = NULL;
	if (!stats || !get_active_runtime(&runtime))
		return false;

	*stats = {};
	stats->configured = runtime->config.has_scene_raster_suppression;
	stats->validated = runtime->scene_raster_suppression_validated;
	stats->requested = runtime->scene_raster_suppression_requested;
	stats->phase_active = runtime->scene_raster_phase_active;
	stats->request_frame = runtime->scene_raster_request_frame;
	stats->current_frame = runtime->frame_state.frame;
	stats->sites.reserve(runtime->scene_raster_suppression_sites.size());
	for (size_t i = 0; i < runtime->scene_raster_suppression_sites.size(); ++i) {
		const ModCallSuppressionSiteRuntime &site =
		        runtime->scene_raster_suppression_sites[i];
		ModCallSuppressionSiteStats site_stats = {};
		site_stats.callsite_reloc = site.callsite_reloc;
		site_stats.executed = site.executed;
		site_stats.skipped = site.skipped;
		stats->sites.push_back(site_stats);
	}
	return true;
}

bool MOD_RequestSafePoint(void)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime) || g_mod.guest_call_active) {
		return false;
	}

	g_mod.safe_point_pending = true;
	return true;
}

void MOD_RunPendingSafePoint(void)
{
	if (g_mod.dynamic_cache_refresh_pending && !g_mod.safe_point_active &&
	    !g_mod.guest_call_active) {
		g_mod.dynamic_cache_refresh_pending = false;
		sync_scene_raster_callsite_hooks();
		refresh_dynamic_cpu_cache();
	}

	if (!g_mod.safe_point_pending || g_mod.safe_point_active ||
	    g_mod.guest_call_active) {
		return;
	}
	if (!cpu.pmode || !cpu.code.big || !cpu.stack.big)
		return;

	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime)) {
		g_mod.safe_point_pending = false;
		return;
	}

	g_mod.safe_point_pending = false;
	g_mod.safe_point_active = true;
	const uint64_t timing_started = MOD_TimingBegin();
	DOSBoxPython_InvokeSafePointCallback();
	MOD_TimingEnd(MOD_TIMING_SAFE_POINT, timing_started);
	g_mod.safe_point_active = false;
}

bool MOD_CallRelocFunction(uint32_t reloc_eip,
                           const ModGuestCallRegisters& input,
                           ModGuestCallRegisters *output)
{
	if (!output || !g_mod.safe_point_active || g_mod.guest_call_active ||
	    !cpu.pmode || !cpu.code.big || !cpu.stack.big ||
	    !ensure_guest_call_stop_callback()) {
		return false;
	}

	uint32_t target_linear = 0;
	if (!translate_reloc_address(reloc_eip, &target_linear))
		return false;

	const uint32_t code_base = static_cast<uint32_t>(SegPhys(cs));
	const uint32_t code_limit = static_cast<uint32_t>(SegLimit(cs));
	const uint32_t stop_linear = static_cast<uint32_t>(
	        CALLBACK_PhysPointer(g_mod.guest_call_stop_callback));
	if (target_linear < code_base || stop_linear < code_base)
		return false;

	const uint32_t target_eip = target_linear - code_base;
	const uint32_t stop_eip = stop_linear - code_base;
	if (target_eip > code_limit || stop_eip > code_limit)
		return false;

	FillFlags();
	const CPU_Regs saved_regs = cpu_regs;
	const Segments saved_segments = Segs;
	const CPUBlock saved_cpu = cpu;
	CPU_Decoder * const saved_decoder = cpudecoder;

	CPU_Push32(stop_eip);
	reg_eax = input.eax;
	reg_ebx = input.ebx;
	reg_ecx = input.ecx;
	reg_edx = input.edx;
	reg_flags &= ~FLAG_DF;
	cpu.direction = 1;
	DestroyConditionFlags();
	reg_eip = target_eip;

	g_mod.guest_call_stopped = false;
	g_mod.guest_call_active = true;
	DOSBOX_RunMachine();

	output->eax = reg_eax;
	output->ebx = reg_ebx;
	output->ecx = reg_ecx;
	output->edx = reg_edx;
	const bool stopped = g_mod.guest_call_stopped;

	g_mod.guest_call_active = false;
	g_mod.guest_call_stopped = false;
	cpu_regs = saved_regs;
	Segs = saved_segments;
	cpu = saved_cpu;
	cpudecoder = saved_decoder;
	DestroyConditionFlags();
	return stopped;
}

int32_t MOD_OnCallsite(uint32_t linear_eip)
{
	if (g_mod.guest_call_active)
		return 0;

	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return 0;

	if (runtime->frame_start_ready && linear_eip == runtime->frame_start_linear)
		update_frame_timing(*runtime);

	const std::unordered_map<uint32_t, ModCallsiteRuntime>::const_iterator it =
	        runtime->callsites.find(linear_eip);
	if (it == runtime->callsites.end())
		return 0;

	if (it->second.scene_raster_phase_leave)
		runtime->scene_raster_phase_active = false;
	if (it->second.scene_raster_phase_enter) {
		const uint64_t current_frame = runtime->frame_state.frame;
		const uint64_t request_frame = runtime->scene_raster_request_frame;
		const bool request_is_fresh =
		        request_frame >= current_frame ||
		        (request_frame < UINT64_MAX &&
		         request_frame + 1u == current_frame);
		runtime->scene_raster_phase_active =
		        runtime->scene_raster_suppression_validated &&
		        runtime->scene_raster_suppression_requested &&
		        request_is_fresh;
		if (!request_is_fresh) {
			runtime->scene_raster_suppression_requested = false;
			g_mod.dynamic_cache_refresh_pending = true;
		}
	}

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

		const uint64_t timing_started = MOD_TimingBegin();
		DOSBoxPython_InvokeHook(runtime->hooks[hook_index].python_hook_id);
		MOD_TimingEnd(MOD_TIMING_PYTHON_HOOK, timing_started);
	}

	const size_t suppression_index =
	        it->second.scene_raster_suppression_index;
	if (suppression_index < runtime->scene_raster_suppression_sites.size()) {
		ModCallSuppressionSiteRuntime &site =
		        runtime->scene_raster_suppression_sites[suppression_index];
		site.executed += 1;
		if (runtime->scene_raster_phase_active) {
			site.skipped += 1;
			return static_cast<int32_t>(
			        0u - static_cast<uint32_t>(site.call_displacement));
		}
	}

	return 0;
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
