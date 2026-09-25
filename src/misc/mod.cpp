#include "mod.h"

#include "callback.h"
#include "control.h"
#include "cpu.h"
#include "dosbox.h"
#include "dosbox_python.h"
#include "dosbox_native_renderer.h"
#include "mw2er_abi.h"
#include "logging.h"
#include "mem.h"
#include "paging.h"
#include "timer.h"
#if C_OPENGL
#include <output/output_opengl.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iterator>
#include <limits>
#include <numeric>
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

struct ModNativeHookRuntime {
	uint32_t event = 0;
	uint32_t kind = 0;
	uint32_t reloc_eip = 0;
};

struct ModCallSuppressionSiteRuntime {
	uint32_t callsite_reloc = 0;
	uint32_t callsite_linear = 0;
	uint32_t target_reloc = 0;
	int32_t call_displacement = 0;
	uint64_t executed = 0;
	uint64_t skipped = 0;
};

static const size_t MOD_NO_CALL_SUPPRESSION_SITE = static_cast<size_t>(-1);

struct ModCallsiteRuntime {
	std::vector<size_t> python_hook_indices = {};
	uint32_t native_renderer_event = 0;
	uint32_t native_renderer_kind = 0;
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
	bool frame_start_defer_ready = false;
	int32_t frame_start_defer_adjustment = 0;
	bool frame_started = false;
	Uint64 frame_counter_start = 0;
	Uint64 frame_counter_last = 0;
	ModFrameState frame_state = {};
	bool process_active = false;
	uint16_t process_psp = 0;
	bool process_start_pending = false;
	uint16_t process_start_psp = 0;
	std::vector<ModHookRuntime> hooks = {};
	std::vector<ModNativeHookRuntime> native_renderer_hooks = {};
	bool has_native_renderer_hooks = false;
	std::unordered_map<uint32_t, ModCallsiteRuntime> callsites = {};
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
	bool safe_point_barrier_active = false;
	uint64_t safe_point_barrier_present_deadline_ns = 0;
	bool safe_point_active = false;
	bool guest_call_active = false;
	bool guest_call_stopped = false;
	Bitu guest_call_stop_callback = 0;
	bool scene_raster_suppression_view_eligible = false;
	std::vector<ModExecutableRuntime> executables = {};
};

ModRuntime g_mod = {};

static void reset_safe_point_barrier(void)
{
	g_mod.safe_point_barrier_active = false;
	g_mod.safe_point_barrier_present_deadline_ns = 0;
}

enum class ModFramePacingPhase {
	Inactive = 0,
	Running = 2,
};

struct ModFramePacingState {
	uint32_t target_fps = 0;
	uint64_t period_ns = 0;
	bool view_eligible = false;
	bool suspended = false;
	ModFramePacingPhase phase = ModFramePacingPhase::Inactive;
	bool waiting = false;
	bool continuous_presentation = false;
	bool ready_since_release = false;
	bool presentation_pending = false;
	uint64_t pending_ready_sequence = 0;
	uint64_t next_deadline_ns = 0;
	uint64_t continuous_presentation_deadline_ns = 0;
	uint64_t deferred_checks = 0;
	uint64_t released_frames = 0;
	uint64_t total_lateness_ns = 0;
	uint64_t maximum_lateness_ns = 0;
	uint64_t interval_released_frames = 0;
	uint64_t interval_lateness_ns = 0;
	uint64_t interval_maximum_lateness_ns = 0;
	uint64_t missed_deadlines = 0;
	uint64_t interval_missed_deadlines = 0;
};

static ModFramePacingState g_frame_pacing = {};

static void timing_write_pacing_summary(bool final);

static void reset_frame_pacing_presentation_state(void)
{
	g_frame_pacing.continuous_presentation = false;
	g_frame_pacing.ready_since_release = false;
	g_frame_pacing.presentation_pending = false;
	g_frame_pacing.pending_ready_sequence = 0;
	g_frame_pacing.continuous_presentation_deadline_ns = 0;
}

static void reset_frame_pacing_runtime_state(void)
{
	if (g_frame_pacing.interval_released_frames > 0)
		timing_write_pacing_summary(true);
	g_frame_pacing.phase = ModFramePacingPhase::Inactive;
	g_frame_pacing.waiting = false;
	reset_frame_pacing_presentation_state();
	g_frame_pacing.next_deadline_ns = 0;
	g_frame_pacing.deferred_checks = 0;
	g_frame_pacing.released_frames = 0;
	g_frame_pacing.total_lateness_ns = 0;
	g_frame_pacing.maximum_lateness_ns = 0;
	g_frame_pacing.interval_released_frames = 0;
	g_frame_pacing.interval_lateness_ns = 0;
	g_frame_pacing.interval_maximum_lateness_ns = 0;
	g_frame_pacing.missed_deadlines = 0;
	g_frame_pacing.interval_missed_deadlines = 0;
}

static const size_t MOD_TIMING_HISTORY_CAPACITY = 256u;
static const uint64_t MOD_TIMING_WARMUP_READY_FRAMES = 180u;
static const uint64_t MOD_TIMING_SUMMARY_INTERVAL_FRAMES = 600u;
static const double MOD_TIMING_SPIKE_THRESHOLD_MS = 25.0;

struct ModTimingAccumulator {
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> elapsed_ns = {};
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> maximum_ns = {};
	double native_extract_ms = 0.0;
	double native_draw_submit_ms = 0.0;
	uint32_t ready_frames = 0;
	uint64_t frame_pacing_sleep_ns = 0;
};

struct ModTimingHistory {
	std::array<double, MOD_TIMING_HISTORY_CAPACITY> values = {};
	size_t count = 0;
	size_t next = 0;
};

enum ModTimingSpikeCause {
	MOD_TIMING_SPIKE_CPU = 0,
	MOD_TIMING_SPIKE_PYTHON_HOOK,
	MOD_TIMING_SPIKE_NATIVE_HOOK,
	MOD_TIMING_SPIKE_PIC,
	MOD_TIMING_SPIKE_SAFE_POINT,
	MOD_TIMING_SPIKE_GFX_EVENTS,
	MOD_TIMING_SPIKE_TIMER,
	MOD_TIMING_SPIKE_FRAME_PACING_SLEEP,
	MOD_TIMING_SPIKE_TICK_OTHER,
	MOD_TIMING_SPIKE_OTHER,
	MOD_TIMING_SPIKE_CAUSE_COUNT,
};

struct ModTimingWindow {
	uint64_t frames = 0;
	uint64_t frame_total_ns = 0;
	uint64_t present_samples = 0;
	uint64_t present_total_ns = 0;
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> category_total_ns = {};
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> category_maximum_ns = {};
	std::array<uint64_t, MOD_TIMING_CATEGORY_COUNT> category_maximum_call_ns = {};
	std::array<uint64_t, MOD_TIMING_SPIKE_CAUSE_COUNT> spike_causes = {};
	uint64_t other_total_ns = 0;
	uint64_t other_maximum_ns = 0;
	uint64_t frame_pacing_sleep_total_ns = 0;
	uint64_t frame_pacing_sleep_maximum_ns = 0;
	double native_extract_total_ms = 0.0;
	double native_extract_maximum_ms = 0.0;
	double native_draw_submit_total_ms = 0.0;
	double native_draw_submit_maximum_ms = 0.0;
	uint64_t worst_frame = 0;
	uint64_t worst_frame_ns = 0;
};

struct ModTimingState {
	uint64_t frame_boundary_ns = 0;
	uint64_t present_boundary_ns = 0;
	uint64_t ready_total = 0;
	uint64_t last_frame = 0;
	uint64_t frame_spikes = 0;
	uint64_t present_spikes = 0;
	ModTimingAccumulator current = {};
	ModTimingHistory frame_history = {};
	ModTimingHistory present_history = {};
	ModTimingWindow window = {};
	ModTimingSummary summary = {};
};

static ModTimingState g_mod_timing = {};
static FILE *g_mod_timing_log = NULL;
// Buffer individual records and flush once per summary window.
static std::array<char, 1024u * 1024u> g_mod_timing_log_buffer = {};

struct ModStartupSample {
	ModTimingAccumulator costs = {};
	uint64_t clock_ns = 0, frame = 0, interval_ns = 0, scope_ns = 0;
	uint64_t ready = 0, released = 0, missed = 0;
	int64_t cycles = 0;
	uint32_t flags = 0, phase = 0;
	char kind = 0;
};

struct ModStartupTrace {
	std::array<ModStartupSample, 8192> samples = {};
	size_t count = 0;
	uint64_t id = 0, start_ns = 0, scene_ns = 0;
	bool recording = false, loading = false;
	const char *stop_reason = "session-end";
};

static ModStartupTrace g_startup_trace;

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

static void timing_log_line(const char *format, ...)
{
	if (!g_mod_timing_log) {
		g_mod_timing_log = fopen("frame_timing.log", "a");
		if (!g_mod_timing_log)
			return;
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
}

static void timing_startup_record(char kind, uint64_t frame, uint64_t now,
	                              uint64_t interval_ns, bool new_mod_frame);

static void timing_startup_flush(const char *reason)
{
	ModStartupTrace &trace = g_startup_trace;
	if (!trace.start_ns)
		return;
	if (trace.recording) {
		const uint64_t now = timing_now_ns();
		const uint64_t origin = g_mod_timing.frame_boundary_ns
		                              ? g_mod_timing.frame_boundary_ns : trace.start_ns;
		timing_startup_record('E', g_mod_timing.last_frame, now,
		                      now - std::max(origin, trace.start_ns), false);
	}
	trace.recording = false;
	timing_log_line(
	        "STARTUP_HOST begin id=%llu clock_ms=%.3f records=%u stop=%s flush=%s target_fps=%u "
	        "components=completed_inclusive_calls_since_frame_boundary "
	        "present_snapshots_overlap_frame_samples=1 "
	        "scope_may_precede_capture=1 "
	        "flags=auto:1,skip_auto:2,suspended:4,waiting:8,continuous:16,view_eligible:32,new_mod_frame:64,loading:128 "
	        "phase=inactive:0,running:2",
	        static_cast<unsigned long long>(trace.id), timing_ns_to_ms(trace.start_ns),
	        static_cast<unsigned int>(trace.count), trace.stop_reason, reason, g_frame_pacing.target_fps);
	for (size_t i = 0; i < trace.count; ++i) {
		const ModStartupSample &s = trace.samples[i];
		const auto ms = [&s](ModTimingCategory category) {
			return timing_ns_to_ms(s.costs.elapsed_ns[category]);
		};
		timing_log_line(
		        "STARTUP_HOST sample id=%llu kind=%c clock_ms=%.3f elapsed_ms=%.3f frame=%llu "
		        "interval_ms=%.3f scope_ms=%.3f ready=%llu scope_ready=%u cycles=%lld flags=%u phase=%u released=%llu missed=%llu "
		        "cpu_ms=%.3f python_ms=%.3f native_ms=%.3f extract_ms=%.3f draw_submit_ms=%.3f "
		        "pic_ms=%.3f safe_ms=%.3f dyn_ms=%.3f events_ms=%.3f timer_ms=%.3f tick_ms=%.3f "
		        "sleep_ms=%.3f pacing_sleep_ms=%.3f compositor_ms=%.3f swap_ms=%.3f swap_call_max_ms=%.3f",
		        static_cast<unsigned long long>(trace.id), s.kind,
		        timing_ns_to_ms(s.clock_ns), timing_ns_to_ms(s.clock_ns - trace.start_ns),
		        static_cast<unsigned long long>(s.frame), timing_ns_to_ms(s.interval_ns),
		        timing_ns_to_ms(s.scope_ns), static_cast<unsigned long long>(s.ready),
		        s.costs.ready_frames, static_cast<long long>(s.cycles), s.flags, s.phase,
		        static_cast<unsigned long long>(s.released), static_cast<unsigned long long>(s.missed),
		        ms(MOD_TIMING_CPU_DECODER), ms(MOD_TIMING_PYTHON_HOOK),
		        ms(MOD_TIMING_NATIVE_RENDERER_HOOK), s.costs.native_extract_ms,
		        s.costs.native_draw_submit_ms, ms(MOD_TIMING_PIC_EVENT),
		        ms(MOD_TIMING_SAFE_POINT), ms(MOD_TIMING_DYNAMIC_COMPILE),
		        ms(MOD_TIMING_GFX_EVENTS), ms(MOD_TIMING_TIMER_TICK),
		        ms(MOD_TIMING_TICK_CONTROL), ms(MOD_TIMING_TICK_SLEEP),
		        timing_ns_to_ms(s.costs.frame_pacing_sleep_ns), ms(MOD_TIMING_COMPOSITOR),
		        ms(MOD_TIMING_SWAP), timing_ns_to_ms(s.costs.maximum_ns[MOD_TIMING_SWAP]));
	}
	timing_log_line("STARTUP_HOST end id=%llu kind_F=frame_end kind_P=post_swap kind_E=partial_end "
	                "first_interval_uses_capture_start=1 capacity=%u",
	                static_cast<unsigned long long>(trace.id),
	                static_cast<unsigned int>(trace.samples.size()));
	if (g_mod_timing_log)
		fflush(g_mod_timing_log);
	trace.start_ns = 0;
	trace.count = 0;
}

static void timing_startup_begin(void)
{
	timing_startup_flush("restart");
	const char *enabled = getenv("MW2_STARTUP_TRACE");
	if (!enabled || enabled[0] != '1' || enabled[1] != '\0')
		return;
	ModStartupTrace &trace = g_startup_trace;
	++trace.id;
	trace.start_ns = timing_now_ns();
	trace.scene_ns = 0;
	trace.loading = false;
	trace.recording = true;
	trace.stop_reason = "session-end";
}

static void timing_startup_record(char kind, uint64_t frame, uint64_t now,
	                              uint64_t interval_ns, bool new_mod_frame)
{
	ModStartupTrace &trace = g_startup_trace;
	if (!trace.recording)
		return;
	if (kind == 'P' && new_mod_frame && !trace.loading && !trace.scene_ns)
		trace.scene_ns = now;
	if (now - trace.start_ns >= 120000000000ull ||
	    (trace.scene_ns && now - trace.scene_ns >= 8000000000ull)) {
		trace.recording = false;
		trace.stop_reason = now - trace.start_ns >= 120000000000ull
		                            ? "time-limit" : "scene-window";
		return;
	}
	if (trace.count == trace.samples.size()) {
		trace.recording = false;
		trace.stop_reason = "capacity-truncated";
		return;
	}
	ModStartupSample &s = trace.samples[trace.count++];
	s.costs = g_mod_timing.current;
	s.clock_ns = now;
	s.frame = frame;
	s.interval_ns = interval_ns;
	const uint64_t origin = g_mod_timing.frame_boundary_ns
	                              ? g_mod_timing.frame_boundary_ns : trace.start_ns;
	s.scope_ns = now >= origin ? now - origin : 0;
	s.ready = g_mod_timing.ready_total;
	s.released = g_frame_pacing.released_frames;
	s.missed = g_frame_pacing.missed_deadlines;
	s.cycles = CPU_CycleMax;
	s.phase = static_cast<uint32_t>(g_frame_pacing.phase);
	s.kind = kind;
	s.flags = (CPU_CycleAutoAdjust ? 1u : 0u) |
	          (CPU_SkipCycleAutoAdjust ? 2u : 0u) |
	          (g_frame_pacing.suspended ? 4u : 0u) |
	          (g_frame_pacing.waiting ? 8u : 0u) |
	          (g_frame_pacing.continuous_presentation ? 16u : 0u) |
	          (g_frame_pacing.view_eligible ? 32u : 0u) |
	          (new_mod_frame ? 64u : 0u) | (trace.loading ? 128u : 0u);
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

static double timing_window_average_ms(const uint64_t total_ns,
	                                    const uint64_t samples)
{
	return samples > 0
	               ? timing_ns_to_ms(total_ns) / static_cast<double>(samples)
	               : 0.0;
}

static ModTimingSpikeCause timing_spike_cause(
	        const ModTimingAccumulator &sample,
	        const uint64_t other_ns)
{
	const uint64_t python_hook_ns = sample.elapsed_ns[MOD_TIMING_PYTHON_HOOK];
	const uint64_t native_hook_ns = sample.elapsed_ns[MOD_TIMING_NATIVE_RENDERER_HOOK];
	const uint64_t hook_ns = python_hook_ns + native_hook_ns;
	const uint64_t cpu_ns = sample.elapsed_ns[MOD_TIMING_CPU_DECODER] > hook_ns
	                                ? sample.elapsed_ns[MOD_TIMING_CPU_DECODER] - hook_ns
	                                : 0;
	const uint64_t pacing_sleep_ns = sample.frame_pacing_sleep_ns;
	const uint64_t tick_ns = sample.elapsed_ns[MOD_TIMING_TICK_CONTROL] >
	                                 pacing_sleep_ns
	                                 ? sample.elapsed_ns[MOD_TIMING_TICK_CONTROL] -
	                                           pacing_sleep_ns
	                                 : 0;
	const std::array<uint64_t, MOD_TIMING_SPIKE_CAUSE_COUNT> durations = {
	        cpu_ns,
	        python_hook_ns,
	        native_hook_ns,
	        sample.elapsed_ns[MOD_TIMING_PIC_EVENT],
	        sample.elapsed_ns[MOD_TIMING_SAFE_POINT],
	        sample.elapsed_ns[MOD_TIMING_GFX_EVENTS],
	        sample.elapsed_ns[MOD_TIMING_TIMER_TICK],
	        pacing_sleep_ns,
	        tick_ns,
	        other_ns,
	};
	return static_cast<ModTimingSpikeCause>(
	        std::distance(durations.begin(),
	                      std::max_element(durations.begin(), durations.end())));
}

static void timing_window_add(const uint64_t frame,
	                          const uint64_t frame_ns,
	                          const ModTimingAccumulator &sample)
{
	ModTimingWindow &window = g_mod_timing.window;
	window.frames++;
	window.frame_total_ns += frame_ns;
	for (size_t i = 0; i < MOD_TIMING_CATEGORY_COUNT; ++i) {
		window.category_total_ns[i] += sample.elapsed_ns[i];
		window.category_maximum_ns[i] = std::max(
		        window.category_maximum_ns[i], sample.elapsed_ns[i]);
		window.category_maximum_call_ns[i] = std::max(
		        window.category_maximum_call_ns[i], sample.maximum_ns[i]);
	}

	window.native_extract_total_ms += sample.native_extract_ms;
	window.native_extract_maximum_ms = std::max(
	        window.native_extract_maximum_ms, sample.native_extract_ms);
	window.native_draw_submit_total_ms += sample.native_draw_submit_ms;
	window.native_draw_submit_maximum_ms = std::max(
	        window.native_draw_submit_maximum_ms, sample.native_draw_submit_ms);

	const uint64_t accounted_ns =
	        sample.elapsed_ns[MOD_TIMING_CPU_DECODER] +
	        sample.elapsed_ns[MOD_TIMING_PIC_EVENT] +
	        sample.elapsed_ns[MOD_TIMING_SAFE_POINT] +
	        sample.elapsed_ns[MOD_TIMING_GFX_EVENTS] +
	        sample.elapsed_ns[MOD_TIMING_TIMER_TICK] +
	        sample.elapsed_ns[MOD_TIMING_TICK_CONTROL];
	const uint64_t other_ns = frame_ns > accounted_ns ? frame_ns - accounted_ns : 0;
	window.other_total_ns += other_ns;
	window.other_maximum_ns = std::max(window.other_maximum_ns, other_ns);
	window.frame_pacing_sleep_total_ns += sample.frame_pacing_sleep_ns;
	window.frame_pacing_sleep_maximum_ns = std::max(
	        window.frame_pacing_sleep_maximum_ns,
	        sample.frame_pacing_sleep_ns);

	if (frame_ns >= static_cast<uint64_t>(MOD_TIMING_SPIKE_THRESHOLD_MS *
	                                     1000000.0)) {
		const ModTimingSpikeCause cause = timing_spike_cause(sample, other_ns);
		window.spike_causes[cause]++;
	}
	if (frame_ns >= window.worst_frame_ns) {
		window.worst_frame = frame;
		window.worst_frame_ns = frame_ns;
	}
}

static void timing_write_window(const uint64_t frame, const bool final)
{
	ModTimingWindow &window = g_mod_timing.window;
	if (window.frames == 0)
		return;

	refresh_timing_summary();
	const double frame_average_ms = timing_window_average_ms(
	        window.frame_total_ns, window.frames);
	const double present_average_ms = timing_window_average_ms(
	        window.present_total_ns, window.present_samples);
	timing_log_line(
	        "FRAME_TIMING summary frame=%llu final=%u window_frames=%llu ready_warmup=%llu samples=%u frame_ms_avg=%.3f frame_fps_avg=%.1f frame_ms_p50=%.3f frame_ms_p95=%.3f frame_ms_p99=%.3f frame_ms_max=%.3f window_worst_frame=%llu window_frame_ms_max=%.3f present_samples=%llu present_ms_avg=%.3f present_fps_avg=%.1f present_ms_p50=%.3f present_ms_p95=%.3f present_ms_p99=%.3f present_ms_max=%.3f frame_spikes=%llu present_spikes=%llu",
	        static_cast<unsigned long long>(frame),
	        final ? 1u : 0u,
	        static_cast<unsigned long long>(window.frames),
	        static_cast<unsigned long long>(MOD_TIMING_WARMUP_READY_FRAMES),
	        static_cast<unsigned int>(g_mod_timing.frame_history.count),
	        frame_average_ms,
	        frame_average_ms > 0.0 ? 1000.0 / frame_average_ms : 0.0,
	        g_mod_timing.summary.frame_p50_ms,
	        g_mod_timing.summary.frame_p95_ms,
	        g_mod_timing.summary.frame_p99_ms,
	        g_mod_timing.summary.frame_max_ms,
	        static_cast<unsigned long long>(window.worst_frame),
	        timing_ns_to_ms(window.worst_frame_ns),
	        static_cast<unsigned long long>(window.present_samples),
	        present_average_ms,
	        present_average_ms > 0.0 ? 1000.0 / present_average_ms : 0.0,
	        g_mod_timing.summary.present_p50_ms,
	        g_mod_timing.summary.present_p95_ms,
	        g_mod_timing.summary.present_p99_ms,
	        g_mod_timing.summary.present_max_ms,
	        static_cast<unsigned long long>(g_mod_timing.frame_spikes),
	        static_cast<unsigned long long>(g_mod_timing.present_spikes));

	const uint64_t samples = window.frames;
	const auto average = [&window, samples](const ModTimingCategory category) {
		return timing_window_average_ms(window.category_total_ns[category], samples);
	};
	const auto maximum = [&window](const ModTimingCategory category) {
		return timing_ns_to_ms(window.category_maximum_ns[category]);
	};
	timing_log_line(
	        "FRAME_TIMING components frame=%llu samples=%llu cpu_ms_avg=%.3f cpu_ms_max=%.3f hook_ms_avg=%.3f hook_ms_max=%.3f native_hook_ms_avg=%.3f native_hook_ms_max=%.3f native_extract_ms_avg=%.3f native_extract_ms_max=%.3f native_draw_submit_ms_avg=%.3f native_draw_submit_ms_max=%.3f pic_ms_avg=%.3f pic_ms_max=%.3f safe_ms_avg=%.3f safe_ms_max=%.3f timer_ms_avg=%.3f timer_ms_max=%.3f dyn_ms_avg=%.3f dyn_ms_max=%.3f events_ms_avg=%.3f events_ms_max=%.3f events_call_ms_max=%.3f tick_ms_avg=%.3f tick_ms_max=%.3f sleep_ms_avg=%.3f sleep_ms_max=%.3f pacing_sleep_ms_avg=%.3f pacing_sleep_ms_max=%.3f compositor_ms_avg=%.3f compositor_ms_max=%.3f swap_ms_avg=%.3f swap_ms_max=%.3f other_ms_avg=%.3f other_ms_max=%.3f",
	        static_cast<unsigned long long>(frame),
	        static_cast<unsigned long long>(samples),
	        average(MOD_TIMING_CPU_DECODER), maximum(MOD_TIMING_CPU_DECODER),
	        average(MOD_TIMING_PYTHON_HOOK), maximum(MOD_TIMING_PYTHON_HOOK),
	        average(MOD_TIMING_NATIVE_RENDERER_HOOK),
	        maximum(MOD_TIMING_NATIVE_RENDERER_HOOK),
	        window.native_extract_total_ms / static_cast<double>(samples),
	        window.native_extract_maximum_ms,
	        window.native_draw_submit_total_ms / static_cast<double>(samples),
	        window.native_draw_submit_maximum_ms,
	        average(MOD_TIMING_PIC_EVENT), maximum(MOD_TIMING_PIC_EVENT),
	        average(MOD_TIMING_SAFE_POINT), maximum(MOD_TIMING_SAFE_POINT),
	        average(MOD_TIMING_TIMER_TICK), maximum(MOD_TIMING_TIMER_TICK),
	        average(MOD_TIMING_DYNAMIC_COMPILE), maximum(MOD_TIMING_DYNAMIC_COMPILE),
	        average(MOD_TIMING_GFX_EVENTS), maximum(MOD_TIMING_GFX_EVENTS),
	        timing_ns_to_ms(window.category_maximum_call_ns[MOD_TIMING_GFX_EVENTS]),
	        average(MOD_TIMING_TICK_CONTROL), maximum(MOD_TIMING_TICK_CONTROL),
	        average(MOD_TIMING_TICK_SLEEP), maximum(MOD_TIMING_TICK_SLEEP),
	        timing_window_average_ms(window.frame_pacing_sleep_total_ns, samples),
	        timing_ns_to_ms(window.frame_pacing_sleep_maximum_ns),
	        average(MOD_TIMING_COMPOSITOR), maximum(MOD_TIMING_COMPOSITOR),
	        average(MOD_TIMING_SWAP), maximum(MOD_TIMING_SWAP),
	        timing_window_average_ms(window.other_total_ns, samples),
	        timing_ns_to_ms(window.other_maximum_ns));

	timing_log_line(
	        "FRAME_TIMING causes frame=%llu spikes=%llu cpu=%llu hook=%llu native_hook=%llu pic=%llu safe=%llu events=%llu timer=%llu pacing_sleep=%llu tick_other=%llu other=%llu",
	        static_cast<unsigned long long>(frame),
	        static_cast<unsigned long long>(
	                std::accumulate(window.spike_causes.begin(),
	                                window.spike_causes.end(), uint64_t{0})),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_CPU]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_PYTHON_HOOK]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_NATIVE_HOOK]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_PIC]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_SAFE_POINT]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_GFX_EVENTS]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_TIMER]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_FRAME_PACING_SLEEP]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_TICK_OTHER]),
	        static_cast<unsigned long long>(window.spike_causes[MOD_TIMING_SPIKE_OTHER]));

	if (g_mod_timing_log)
		fflush(g_mod_timing_log);
	window = {};
}

static void timing_frame_boundary(const uint64_t frame)
{
	const uint64_t now = timing_now_ns();
	if (g_startup_trace.recording) {
		const uint64_t origin = g_mod_timing.frame_boundary_ns
		                              ? g_mod_timing.frame_boundary_ns : g_startup_trace.start_ns;
		timing_startup_record('F', frame > 0 ? frame - 1u : 0u,
		                      now, now - std::max(origin, g_startup_trace.start_ns), false);
	}
	if (g_mod_timing.frame_boundary_ns == 0) {
		g_mod_timing.frame_boundary_ns = now;
		g_mod_timing.last_frame = frame;
		g_mod_timing.current = {};
		return;
	}

	const uint64_t frame_ns = now - g_mod_timing.frame_boundary_ns;
	const double frame_ms = timing_ns_to_ms(frame_ns);
	g_mod_timing.frame_boundary_ns = now;
	g_mod_timing.last_frame = frame;
	const ModTimingAccumulator sample = g_mod_timing.current;
	g_mod_timing.current = {};

	const bool steady_state =
	        g_mod_timing.ready_total > MOD_TIMING_WARMUP_READY_FRAMES &&
	        sample.ready_frames != 0;
	if (steady_state) {
		timing_history_add(g_mod_timing.frame_history, frame_ms);
		timing_window_add(frame > 0 ? frame - 1u : 0u, frame_ns, sample);
		if (frame_ms >= MOD_TIMING_SPIKE_THRESHOLD_MS)
			g_mod_timing.frame_spikes++;

		if (frame % MOD_TIMING_SUMMARY_INTERVAL_FRAMES == 0u) {
			timing_write_window(frame, false);
		} else if (g_mod_timing.frame_history.count % 120u == 0u) {
			refresh_timing_summary();
		}
	}
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

static uint32_t get_configured_mod_renderer_target_fps(const Config& config)
{
	const Section_prop *render_section =
	        dynamic_cast<const Section_prop *>(config.GetSection("render"));
	if (!render_section)
		return 0;

	const int configured_fps =
	        render_section->Get_int("mod renderer target fps");
	return configured_fps > 0 ? static_cast<uint32_t>(configured_fps) : 0;
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
	    callsite.native_renderer_event == 0 &&
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
	timing_startup_flush("runtime-reset");
	if (g_mod_timing.window.frames > 0)
		timing_write_window(g_mod_timing.last_frame, true);
	runtime.frame_started = false;
	runtime.frame_counter_start = 0;
	runtime.frame_counter_last = 0;
	runtime.frame_state = {};
	reset_frame_pacing_runtime_state();
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
	runtime.frame_start_defer_ready = false;
	runtime.frame_start_defer_adjustment = 0;
	runtime.callsites.clear();
	runtime.scene_raster_suppression_sites.clear();
	runtime.scene_raster_suppression_validated = false;
	runtime.scene_raster_callsite_hooks_active = false;
	runtime.scene_raster_phase_enter_linear = 0;
	runtime.scene_raster_phase_leave_linear = 0;
	reset_scene_raster_suppression(runtime, true);

	if (!runtime.delta_ready)
		return;

	if (runtime.config.has_frame_start) {
		uint32_t frame_start_linear = 0;
		if (apply_delta(runtime.config.frame_start_reloc, runtime.delta, &frame_start_linear)) {
			runtime.frame_start_linear = frame_start_linear;
			runtime.frame_start_ready = true;

			uint8_t opcode = 0;
			uint32_t raw_displacement = 0;
			if (!mem_readb_checked(frame_start_linear, &opcode) &&
			    opcode == 0xe8u &&
			    !mem_readd_checked(frame_start_linear + 1u,
			                      &raw_displacement)) {
				const int32_t displacement =
				        static_cast<int32_t>(raw_displacement);
				const int64_t call_target =
				        static_cast<int64_t>(frame_start_linear) + 5ll +
				        static_cast<int64_t>(displacement);
				const int64_t adjustment =
				        static_cast<int64_t>(frame_start_linear) - call_target;
				if (adjustment >= std::numeric_limits<int32_t>::min() &&
				    adjustment <= std::numeric_limits<int32_t>::max() &&
				    adjustment != 0) {
					runtime.frame_start_defer_adjustment =
					        static_cast<int32_t>(adjustment);
					runtime.frame_start_defer_ready = true;
				}
			}

			if (g_frame_pacing.target_fps > 0 &&
			    !runtime.frame_start_defer_ready) {
				LOG_MSG("MOD ERROR: %s frame pacing disabled because frame_start is not a readable near call",
				        runtime.config.name.c_str());
			}
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

	for (size_t i = 0; i < runtime.native_renderer_hooks.size(); ++i) {
		const ModNativeHookRuntime& hook = runtime.native_renderer_hooks[i];
		uint32_t linear_eip = 0;
		if (apply_delta(hook.reloc_eip, runtime.delta, &linear_eip)) {
			runtime.callsites[linear_eip].native_renderer_event = hook.event;
			runtime.callsites[linear_eip].native_renderer_kind = hook.kind;
		} else {
			LOG_MSG("MOD ERROR: native renderer hook has invalid runtime address");
		}
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
	set_scene_raster_callsite_hooks(runtime, false);
	reset_scene_raster_suppression(runtime, true);
	update_fast_enabled();
	refresh_dynamic_cpu_cache();
	timing_startup_begin();
	const char *variant = getenv("MW2_PERF_VARIANT");
	timing_log_line(
	        "FRAME_TIMING session executable=%s variant=%s format=condensed-v1 warmup_ready_frames=%llu spike_threshold_ms=%.3f history_capacity=%u summary_interval_frames=%llu",
	        runtime.config.name.c_str(),
	        variant && variant[0] ? variant : "unspecified",
	        static_cast<unsigned long long>(MOD_TIMING_WARMUP_READY_FRAMES),
	        MOD_TIMING_SPIKE_THRESHOLD_MS,
	        static_cast<unsigned int>(MOD_TIMING_HISTORY_CAPACITY),
	        static_cast<unsigned long long>(MOD_TIMING_SUMMARY_INTERVAL_FRAMES));

	LOG_MSG("MOD: %s active on PSP 0x%04X",
	        runtime.config.name.c_str(),
	        static_cast<unsigned int>(pspseg));
	if (runtime.has_native_renderer_hooks &&
	    !DOSBoxNativeRenderer_MissionBegin(runtime.delta)) {
		LOG_MSG("NATIVE RENDERER: mission start deferred or unavailable");
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

static void attach_native_renderer_hook(void)
{
	const size_t count = DOSBoxNativeRenderer_GetHookCount();
	for (size_t binding = 0; binding < count; ++binding) {
		std::string exe_name_upper;
		uint32_t event = 0;
		uint32_t kind = 0;
		uint32_t reloc_eip = 0;
		if (!DOSBoxNativeRenderer_GetHook(
		            binding, &exe_name_upper, &event, &kind, &reloc_eip))
			continue;
		bool matched = false;
		for (size_t i = 0; i < g_mod.executables.size(); ++i) {
			ModExecutableRuntime &runtime = g_mod.executables[i];
			if (runtime.config.name_upper != exe_name_upper)
				continue;
			runtime.has_native_renderer_hooks = true;
			ModNativeHookRuntime hook = {};
			hook.event = event;
			hook.kind = kind;
			hook.reloc_eip = reloc_eip;
			runtime.native_renderer_hooks.push_back(hook);
			matched = true;
			break;
		}
		if (!matched) {
			LOG_MSG("NATIVE RENDERER ERROR: descriptor references unknown executable %s",
			        exe_name_upper.c_str());
			DOSBoxNativeRenderer_Shutdown();
			return;
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

static bool frame_pacing_runtime_ready(ModExecutableRuntime **runtime)
{
	if (g_frame_pacing.target_fps == 0) {
		return false;
	}

	ModExecutableRuntime *active = NULL;
	if (!get_active_runtime(&active) || !active->frame_start_defer_ready)
		return false;

	if (runtime)
		*runtime = active;
	return true;
}

static bool frame_pacing_presentation_ready(void)
{
	return g_frame_pacing.view_eligible && !g_frame_pacing.suspended &&
	       frame_pacing_runtime_ready(NULL);
}

static void timing_write_pacing_summary(const bool final)
{
	if (g_frame_pacing.interval_released_frames == 0 ||
	    g_frame_pacing.released_frames == 0) {
		return;
	}
	const double average_lateness_ms =
	        timing_window_average_ms(g_frame_pacing.total_lateness_ns,
	                                 g_frame_pacing.released_frames);
	const double interval_average_lateness_ms =
	        timing_window_average_ms(
	                g_frame_pacing.interval_lateness_ns,
	                g_frame_pacing.interval_released_frames);
	timing_log_line(
	        "FRAME_PACING summary final=%u target_fps=%u released=%llu deferred_checks=%llu average_lateness_ms=%.3f maximum_lateness_ms=%.3f interval_released=%llu interval_average_lateness_ms=%.3f interval_maximum_lateness_ms=%.3f missed_deadlines=%llu interval_missed_deadlines=%llu",
	        final ? 1u : 0u,
	        static_cast<unsigned int>(g_frame_pacing.target_fps),
	        static_cast<unsigned long long>(g_frame_pacing.released_frames),
	        static_cast<unsigned long long>(g_frame_pacing.deferred_checks),
	        average_lateness_ms,
	        timing_ns_to_ms(g_frame_pacing.maximum_lateness_ns),
	        static_cast<unsigned long long>(
	                g_frame_pacing.interval_released_frames),
	        interval_average_lateness_ms,
	        timing_ns_to_ms(g_frame_pacing.interval_maximum_lateness_ns),
	        static_cast<unsigned long long>(g_frame_pacing.missed_deadlines),
	        static_cast<unsigned long long>(
	                g_frame_pacing.interval_missed_deadlines));
	g_frame_pacing.interval_released_frames = 0;
	g_frame_pacing.interval_lateness_ns = 0;
	g_frame_pacing.interval_maximum_lateness_ns = 0;
	g_frame_pacing.interval_missed_deadlines = 0;
}

static int32_t frame_pacing_on_frame_start(ModExecutableRuntime &runtime)
{
	ModExecutableRuntime *active = NULL;
	if (!frame_pacing_runtime_ready(&active) || active != &runtime) {
		reset_frame_pacing_runtime_state();
		return 0;
	}

	const uint64_t now = timing_now_ns();
	if (g_frame_pacing.phase == ModFramePacingPhase::Inactive) {
		g_frame_pacing.phase = ModFramePacingPhase::Running;
		g_frame_pacing.next_deadline_ns = now + g_frame_pacing.period_ns;
		LOG_MSG("MOD: guest frame pacing active at %u FPS",
		        static_cast<unsigned int>(g_frame_pacing.target_fps));
		return 0;
	}

	if (now < g_frame_pacing.next_deadline_ns) {
		g_frame_pacing.waiting = true;
		g_frame_pacing.deferred_checks++;
		CPU_Cycles = 0;
		return runtime.frame_start_defer_adjustment;
	}

	if (!g_frame_pacing.ready_since_release && !g_frame_pacing.continuous_presentation)
		reset_frame_pacing_presentation_state();
	g_frame_pacing.ready_since_release = false;
	g_frame_pacing.waiting = false;
	const uint64_t lateness_ns = now - g_frame_pacing.next_deadline_ns;
	g_frame_pacing.total_lateness_ns += lateness_ns;
	g_frame_pacing.maximum_lateness_ns = std::max(
	        g_frame_pacing.maximum_lateness_ns, lateness_ns);
	g_frame_pacing.released_frames++;
	g_frame_pacing.interval_lateness_ns += lateness_ns;
	g_frame_pacing.interval_maximum_lateness_ns = std::max(
	        g_frame_pacing.interval_maximum_lateness_ns, lateness_ns);
	g_frame_pacing.interval_released_frames++;
	uint64_t deadlines_advanced = 0;
	do {
		g_frame_pacing.next_deadline_ns += g_frame_pacing.period_ns;
		deadlines_advanced++;
	} while (g_frame_pacing.next_deadline_ns <= now);
	const uint64_t missed_deadlines = deadlines_advanced > 0
	                                          ? deadlines_advanced - 1u
	                                          : 0u;
	g_frame_pacing.missed_deadlines += missed_deadlines;
	g_frame_pacing.interval_missed_deadlines += missed_deadlines;

	if ((g_frame_pacing.released_frames %
	     MOD_TIMING_SUMMARY_INTERVAL_FRAMES) == 0u) {
		timing_write_pacing_summary(false);
	}

	return 0;
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
	return elapsed;
}

void MOD_TimingRecordFramePacingSleep(const uint64_t elapsed_ns)
{
	if (!g_mod.fast_enabled || elapsed_ns == 0)
		return;
	g_mod_timing.current.frame_pacing_sleep_ns += elapsed_ns;
}

void MOD_TimingCountModFrameReady(void)
{
	if (!g_mod.fast_enabled)
		return;
	g_mod_timing.current.ready_frames++;
	g_mod_timing.ready_total++;
}

void MOD_TimingPresentationBoundary(const bool new_mod_frame)
{
	if (!g_mod.fast_enabled)
		return;

	ModTimingState &timing = g_mod_timing;
	const uint64_t now = timing_now_ns();
	if (g_startup_trace.recording) {
		const uint64_t origin = timing.present_boundary_ns
		                              ? timing.present_boundary_ns : g_startup_trace.start_ns;
		timing_startup_record('P', timing.last_frame, now,
		                      now - std::max(origin, g_startup_trace.start_ns), new_mod_frame);
	}
	if (timing.present_boundary_ns == 0) {
		timing.present_boundary_ns = now;
		return;
	}

	const uint64_t present_ns = now - timing.present_boundary_ns;
	const double present_ms = timing_ns_to_ms(present_ns);
	timing.present_boundary_ns = now;
	if (!new_mod_frame ||
	    timing.ready_total <= MOD_TIMING_WARMUP_READY_FRAMES) {
		return;
	}

	timing_history_add(timing.present_history, present_ms);
	timing.window.present_samples++;
	timing.window.present_total_ns += present_ns;
	if (present_ms >= MOD_TIMING_SPIKE_THRESHOLD_MS)
		timing.present_spikes++;
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
	g_frame_pacing = {};
	g_frame_pacing.target_fps =
	        get_configured_mod_renderer_target_fps(config);
	if (g_frame_pacing.target_fps > 0) {
		g_frame_pacing.period_ns =
		        1000000000ull / g_frame_pacing.target_fps;
	}
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

	if (!DOSBoxNativeRenderer_Init(config))
		return false;
	if (DOSBoxNativeRenderer_Available() &&
	    DOSBoxPython_OpenGLRendererAvailable()) {
		LOG_MSG("NATIVE RENDERER ERROR: Python and native renderers cannot be active together; native renderer disabled");
		DOSBoxNativeRenderer_Shutdown();
	} else {
		attach_native_renderer_hook();
	}

	g_mod.initialized = true;
	return true;
}

void MOD_Shutdown(void)
{
	timing_startup_flush("shutdown");
	if (g_mod_timing.window.frames > 0)
		timing_write_window(g_mod_timing.last_frame, true);
	if (g_frame_pacing.interval_released_frames > 0)
		timing_write_pacing_summary(true);
	if (g_mod.guest_call_stop_callback != 0)
		CALLBACK_DeAllocate(g_mod.guest_call_stop_callback);
	g_mod = {};
	g_frame_pacing = {};
	DOSBoxNativeRenderer_Shutdown();
	DOSBoxPython_Shutdown();
	close_mod_timing_log();
}

void MOD_OnOpenFile(const char *name, unsigned short handle)
{
	if (!g_mod.initialized || !name || g_mod.executables.empty())
		return;
	DOSBoxNativeRenderer_OnFileOpened(name);

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
			if (runtime.has_native_renderer_hooks)
				DOSBoxNativeRenderer_MissionEnd();
			g_mod.safe_point_pending = false;
			reset_safe_point_barrier();
			runtime.process_active = false;
			runtime.process_psp = 0;
			runtime.process_start_pending = false;
			runtime.process_start_psp = 0;
			g_mod.active_executable_valid = false;
			reset_runtime_frame_state(runtime);
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

const char *MOD_GetRendererSourceName(void)
{
	if (DOSBoxNativeRenderer_Available())
		return DOSBoxNativeRenderer_GetSourceName();
	if (DOSBoxPython_OpenGLRendererAvailable())
		return DOSBoxPython_GetOpenGLRendererSourceName();
	return "";
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
	if (enabled && !g_mod.scene_raster_suppression_view_eligible)
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

void MOD_SetSceneRasterSuppressionViewEligible(bool eligible)
{
	if (g_mod.scene_raster_suppression_view_eligible == eligible)
		return;
	g_mod.scene_raster_suppression_view_eligible = eligible;
	if (!eligible)
		MOD_DisableSceneRasterSuppression();
}

bool MOD_CallsiteCanBeSuppressed(uint32_t linear_eip)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime)) {
		return false;
	}
	if (g_frame_pacing.target_fps > 0 &&
	    runtime->frame_start_defer_ready &&
	    linear_eip == runtime->frame_start_linear) {
		return true;
	}
	if (!runtime->scene_raster_suppression_validated)
		return false;

	const std::unordered_map<uint32_t, ModCallsiteRuntime>::const_iterator it =
	        runtime->callsites.find(linear_eip);
	return runtime->scene_raster_suppression_requested &&
	       it != runtime->callsites.end() &&
	       it->second.scene_raster_suppression_index <
	               runtime->scene_raster_suppression_sites.size();
}

void MOD_SetFramePacingViewEligible(bool eligible)
{
	if (g_frame_pacing.view_eligible == eligible)
		return;
	g_frame_pacing.view_eligible = eligible;
	reset_frame_pacing_presentation_state();
}

bool MOD_SetFramePacingSuspended(bool suspended)
{
	// A new loading presentation can begin without restarting the executable.
	if (g_startup_trace.start_ns && suspended && !g_startup_trace.loading &&
	    g_startup_trace.scene_ns)
		timing_startup_begin();
	g_startup_trace.loading = suspended;
	if (g_frame_pacing.target_fps == 0)
		return false;
	if (g_frame_pacing.suspended == suspended)
		return true;
	g_frame_pacing.suspended = suspended;
	reset_frame_pacing_presentation_state();
	return true;
}

bool MOD_SetFramePacingContinuousPresentation(bool active)
{
	if (g_frame_pacing.target_fps == 0)
		return false;

	if (!active) {
		g_frame_pacing.continuous_presentation = false;
		g_frame_pacing.continuous_presentation_deadline_ns = 0;
		return true;
	}

	if (g_frame_pacing.phase != ModFramePacingPhase::Running ||
	    !frame_pacing_presentation_ready() ||
	    g_frame_pacing.pending_ready_sequence == 0) {
		return false;
	}

	if (!g_frame_pacing.continuous_presentation) {
		const uint64_t now = timing_now_ns();
		g_frame_pacing.continuous_presentation_deadline_ns =
		        g_frame_pacing.next_deadline_ns > now
		                ? g_frame_pacing.next_deadline_ns
		                : now + g_frame_pacing.period_ns;
		g_frame_pacing.continuous_presentation = true;
	}
	return true;
}

// Read-only diagnostics: active, suspended, waiting, continuous, barrier,
// pending safe point, guest call, eligible view, running pacer, active safe point.
uint32_t MOD_FramePacingAuditFlags(void)
{
    return (MOD_RenderActive() ? 1u : 0u) |
           (g_frame_pacing.suspended ? 2u : 0u) |
           (g_frame_pacing.waiting ? 4u : 0u) |
           (g_frame_pacing.continuous_presentation ? 8u : 0u) |
           (MOD_SafePointBarrierActive() ? 16u : 0u) |
           (MOD_SafePointPending() ? 32u : 0u) |
           (MOD_GuestCallActive() ? 64u : 0u) |
           (g_frame_pacing.view_eligible ? 128u : 0u) |
           (g_frame_pacing.phase == ModFramePacingPhase::Running ? 256u : 0u) |
           (g_mod.safe_point_active ? 512u : 0u);
}

bool MOD_FramePacingWaiting(void)
{
	return g_frame_pacing.waiting && frame_pacing_runtime_ready(NULL);
}

bool MOD_FramePacingOwnsPresentation(void)
{
	return g_frame_pacing.phase == ModFramePacingPhase::Running &&
	       g_frame_pacing.pending_ready_sequence != 0 &&
	       frame_pacing_presentation_ready();
}

void MOD_FramePacingNotifyReady(uint64_t ready_sequence)
{
	if (ready_sequence == 0 || !frame_pacing_presentation_ready() ||
	    g_frame_pacing.phase == ModFramePacingPhase::Inactive) {
		return;
	}

	g_frame_pacing.ready_since_release = true;
	g_frame_pacing.presentation_pending = true;
	g_frame_pacing.pending_ready_sequence = ready_sequence;
	CPU_Cycles = 0;
}

bool MOD_FramePacingTakePresentation(uint64_t *ready_sequence)
{
	if (!ready_sequence || !frame_pacing_presentation_ready()) {
		return false;
	}

	if (g_frame_pacing.continuous_presentation) {
		const uint64_t now = timing_now_ns();
		if (now < g_frame_pacing.continuous_presentation_deadline_ns)
			return false;

		*ready_sequence = g_frame_pacing.pending_ready_sequence;
		g_frame_pacing.presentation_pending = false;
		do {
			g_frame_pacing.continuous_presentation_deadline_ns +=
			        g_frame_pacing.period_ns;
		} while (g_frame_pacing.continuous_presentation_deadline_ns <=
		         now);
		return true;
	}

	if (!g_frame_pacing.presentation_pending)
		return false;

	*ready_sequence = g_frame_pacing.pending_ready_sequence;
	g_frame_pacing.presentation_pending = false;
	return true;
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

bool MOD_SetSafePointBarrier(bool active)
{
	if (!active) {
		reset_safe_point_barrier();
		return true;
	}

	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime) || g_mod.guest_call_active)
		return false;

	g_mod.safe_point_barrier_active = true;
	g_mod.safe_point_barrier_present_deadline_ns = timing_now_ns();
	CPU_Cycles = 0;
	return true;
}

bool MOD_SafePointBarrierActive(void)
{
	return g_mod.safe_point_barrier_active;
}

bool MOD_SafePointPending(void)
{
	return g_mod.safe_point_pending && !g_mod.safe_point_active &&
	       !g_mod.guest_call_active && cpu.pmode && cpu.code.big &&
	       cpu.stack.big;
}

bool MOD_SafePointBarrierPresentationDue(void)
{
	if (!g_mod.safe_point_barrier_active)
		return false;

	// Always redraw after the final batch before releasing the guest. This
	// prevents a resource upload from leaving an incomplete backbuffer for the
	// next swap.
	if (!g_mod.safe_point_pending)
		return true;

	const uint64_t now = timing_now_ns();
	if (g_mod.safe_point_barrier_present_deadline_ns > now)
		return false;

	const uint64_t period_ns = g_frame_pacing.period_ns > 0
	                                   ? g_frame_pacing.period_ns
	                                   : 1000000000ull / 60ull;
	do {
		g_mod.safe_point_barrier_present_deadline_ns += period_ns;
	} while (g_mod.safe_point_barrier_present_deadline_ns <= now);
	return true;
}

bool MOD_RequestSafePoint(void)
{
	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime) || g_mod.guest_call_active) {
		return false;
	}

	g_mod.safe_point_pending = true;
	// A request made by a guest-code hook must run before the decoder resumes
	// at the hook's continuation address. End the current decoder slice so the
	// main loop services the pending callback first.
	if (!g_mod.safe_point_active)
		CPU_Cycles = 0;
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
		reset_safe_point_barrier();
		return;
	}

	g_mod.safe_point_pending = false;
	g_mod.safe_point_active = true;
	const uint64_t timing_started = MOD_TimingBegin();
	DOSBOX_BeginAutoCycleHostWork();
	try {
		DOSBoxNativeRenderer_ServiceResources();
		DOSBoxPython_InvokeSafePointCallback();
	} catch (...) {
		g_mod.safe_point_active = false;
		DOSBOX_EndAutoCycleHostWork();
		throw;
	}
	MOD_TimingEnd(MOD_TIMING_SAFE_POINT, timing_started);
	g_mod.safe_point_active = false;
	DOSBOX_EndAutoCycleHostWork();
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
	const auto restore_guest_state = [&]() {
		g_mod.guest_call_active = false;
		g_mod.guest_call_stopped = false;
		cpu_regs = saved_regs;
		Segs = saved_segments;
		cpu = saved_cpu;
		cpudecoder = saved_decoder;
		DestroyConditionFlags();
	};

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
	DOSBOX_EndAutoCycleHostWork();
	try {
		DOSBOX_RunMachine();
	} catch (...) {
		DOSBOX_BeginAutoCycleHostWork();
		restore_guest_state();
		throw;
	}
	DOSBOX_BeginAutoCycleHostWork();

	output->eax = reg_eax;
	output->ebx = reg_ebx;
	output->ecx = reg_ecx;
	output->edx = reg_edx;
	const bool stopped = g_mod.guest_call_stopped;

	restore_guest_state();
	return stopped;
}

int32_t MOD_OnCallsite(uint32_t linear_eip)
{
	if (g_mod.guest_call_active)
		return 0;

	ModExecutableRuntime *runtime = NULL;
	if (!get_active_runtime(&runtime))
		return 0;

	if (runtime->frame_start_ready && linear_eip == runtime->frame_start_linear) {
		const int32_t pacing_adjustment =
		        frame_pacing_on_frame_start(*runtime);
		if (pacing_adjustment != 0)
			return pacing_adjustment;
		update_frame_timing(*runtime);
	}

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

	for (size_t i = 0; i < it->second.python_hook_indices.size(); ++i) {
		const size_t hook_index = it->second.python_hook_indices[i];
		if (hook_index >= runtime->hooks.size())
			continue;

		const uint64_t timing_started = MOD_TimingBegin();
		DOSBoxPython_InvokeHook(runtime->hooks[hook_index].python_hook_id);
		MOD_TimingEnd(MOD_TIMING_PYTHON_HOOK, timing_started);
	}

	if (it->second.native_renderer_event != 0) {
		const uint64_t timing_started = MOD_TimingBegin();
		const uint32_t event = it->second.native_renderer_event;
		const uint32_t kind = it->second.native_renderer_kind;
		const bool handled = DOSBoxNativeRenderer_InvokeHook(
		        event, kind, runtime->frame_state, runtime->delta);
		MOD_TimingEnd(MOD_TIMING_NATIVE_RENDERER_HOOK, timing_started);
		if (kind == MW2ER_HOOK_RENDER) {
			double extract_ms = 0.0;
			double draw_submit_ms = 0.0;
			DOSBoxNativeRenderer_GetLastTiming(&extract_ms, &draw_submit_ms);
			g_mod_timing.current.native_extract_ms += extract_ms;
			g_mod_timing.current.native_draw_submit_ms += draw_submit_ms;
		}
		if (kind == MW2ER_HOOK_RENDER && handled) {
			if (g_mod.scene_raster_suppression_view_eligible)
				MOD_SetSceneRasterSuppression(true);
#if C_OPENGL
			OUTPUT_OPENGL_NotifyModFrameReady();
#endif
		} else if (kind == MW2ER_HOOK_RENDER) {
			MOD_SetSceneRasterSuppression(false);
		}
		if (kind == MW2ER_HOOK_COMPOSITOR && handled) {
#if C_OPENGL
			/* Presentation hooks schedule one compositor call. The compositor
			 * then requests further calls while its effect remains active. */
			OUTPUT_OPENGL_RequestModPresentation();
			CPU_Cycles = 0;
#endif
		}
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
