#ifndef DOSBOX_MOD_H
#define DOSBOX_MOD_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

class Config;

struct ModCallSuppressionSiteConfig {
	uint32_t callsite_reloc = 0;
	uint32_t target_reloc = 0;
};

struct ModExecutableConfig {
	std::string name = {};
	std::string name_upper = {};
	std::string landmark_string = {};
	uint32_t landmark_reloc = 0;
	bool has_scan_range = false;
	uint32_t scan_start = 0;
	uint32_t scan_end = 0;
	bool has_frame_start = false;
	uint32_t frame_start_reloc = 0;
	bool has_scene_raster_suppression = false;
	uint32_t scene_raster_phase_enter_reloc = 0;
	uint32_t scene_raster_phase_leave_reloc = 0;
	std::vector<ModCallSuppressionSiteConfig> scene_raster_suppression_sites = {};
};

struct ModCallSuppressionSiteStats {
	uint32_t callsite_reloc = 0;
	uint64_t executed = 0;
	uint64_t skipped = 0;
};

struct ModSceneRasterSuppressionStats {
	bool configured = false;
	bool validated = false;
	bool requested = false;
	bool phase_active = false;
	uint64_t request_frame = 0;
	uint64_t current_frame = 0;
	std::vector<ModCallSuppressionSiteStats> sites = {};
};

struct ModGuestCallRegisters {
	uint32_t eax = 0;
	uint32_t ebx = 0;
	uint32_t ecx = 0;
	uint32_t edx = 0;
};

struct ModFrameState {
	uint64_t frame = 0;
	double time_seconds = 0.0;
	double frame_delta_seconds = 0.0;
};

enum ModTimingCategory {
	MOD_TIMING_CPU_DECODER = 0,
	MOD_TIMING_PIC_EVENT,
	MOD_TIMING_PYTHON_HOOK,
	MOD_TIMING_SAFE_POINT,
	MOD_TIMING_DYNAMIC_COMPILE,
	MOD_TIMING_COMPOSITOR,
	MOD_TIMING_SWAP,
	MOD_TIMING_GFX_EVENTS,
	MOD_TIMING_TIMER_TICK,
	MOD_TIMING_TICK_CONTROL,
	MOD_TIMING_TICK_SLEEP,
	MOD_TIMING_AUTO_CYCLE,
	MOD_TIMING_CATEGORY_COUNT,
};

struct ModTimingSummary {
	bool valid = false;
	double frame_p50_ms = 0.0;
	double frame_p95_ms = 0.0;
	double frame_p99_ms = 0.0;
	double frame_max_ms = 0.0;
	double present_p50_ms = 0.0;
	double present_p95_ms = 0.0;
	double present_p99_ms = 0.0;
	double present_max_ms = 0.0;
	uint64_t frame_spikes = 0;
	uint64_t present_spikes = 0;
};

enum ModRenderViewMode {
	MOD_RENDER_VIEW_GAME_ONLY = 0,
	MOD_RENDER_VIEW_MOD_ONLY = 1,
	MOD_RENDER_VIEW_SIDE_BY_SIDE = 2,
	MOD_RENDER_VIEW_SIDE_BY_SIDE_SUPPRESSED = 3,
};

struct ModOpenGLState {
	uint64_t context_generation = 0;
	uint64_t present_count = 0;
	uint32_t backbuffer_width = 0;
	uint32_t backbuffer_height = 0;
	uint32_t backbuffer_framebuffer = 0;
	uint32_t draw_width = 0;
	uint32_t draw_height = 0;
	uint32_t view_mode = MOD_RENDER_VIEW_SIDE_BY_SIDE;
	uint32_t clip_x = 0;
	uint32_t clip_y = 0;
	uint32_t clip_w = 0;
	uint32_t clip_h = 0;
	uint32_t game_viewport_x = 0;
	uint32_t game_viewport_y = 0;
	uint32_t game_viewport_w = 0;
	uint32_t game_viewport_h = 0;
	uint32_t mod_viewport_x = 0;
	uint32_t mod_viewport_y = 0;
	uint32_t mod_viewport_w = 0;
	uint32_t mod_viewport_h = 0;
};

struct ModPythonHookRegistration {
	size_t hook_id = 0;
	std::string exe_name_upper = {};
	uint32_t reloc_eip = 0;
	std::string kind = {};
	std::string description = {};
	bool render_aware = false;
};

bool MOD_Init(const Config& config);
void MOD_Shutdown(void);
void MOD_OnOpenFile(const char *name, unsigned short handle);
void MOD_OnCloseFile(unsigned short handle);
void MOD_OnExecutableStarted(const char *name, uint16_t pspseg);
void MOD_OnTerminatePSP(uint16_t pspseg, bool tsr, uint8_t exitcode);
bool MOD_FastEnabled(void);
bool MOD_RenderActive(void);
bool MOD_GuestCallActive(void);
int32_t MOD_OnCallsite(uint32_t linear_eip);
bool MOD_CallsiteCanBeSuppressed(uint32_t linear_eip);
bool MOD_SetSceneRasterSuppression(bool enabled);
void MOD_DisableSceneRasterSuppression(void);
bool MOD_GetSceneRasterSuppressionStats(ModSceneRasterSuppressionStats *stats);
bool MOD_RequestSafePoint(void);
void MOD_RunPendingSafePoint(void);
bool MOD_CallRelocFunction(uint32_t reloc_eip,
                           const ModGuestCallRegisters& input,
                           ModGuestCallRegisters *output);
uint64_t MOD_TimingBegin(void);
uint64_t MOD_TimingEnd(ModTimingCategory category, uint64_t started_ns);
void MOD_TimingRecordDecoderSlice(uint64_t elapsed_ns,
                                  int64_t requested_cycles,
                                  int64_t remaining_cycles,
                                  int64_t cycle_max,
                                  bool auto_adjust);
void MOD_TimingRecordAutoCycleAdjustment(int64_t cycle_max_before,
                                         int64_t cycle_max_after,
                                         int32_t ticks_added,
                                         int32_t ticks_scheduled,
                                         int32_t ticks_done);
void MOD_TimingCountNativeFrame(void);
void MOD_TimingCountModFrameReady(void);
void MOD_TimingPresentationBoundary(bool compositor_invoked,
                                    bool new_mod_frame,
                                    uint64_t compositor_ns,
                                    uint64_t swap_ns,
                                    const char *source);
bool MOD_GetTimingSummary(ModTimingSummary *summary);

bool MOD_ReadMemoryU8(uint32_t reloc_addr, uint8_t *value);
bool MOD_ReadMemoryU16(uint32_t reloc_addr, uint16_t *value);
bool MOD_ReadMemoryU32(uint32_t reloc_addr, uint32_t *value);
bool MOD_ReadMemoryI32(uint32_t reloc_addr, int32_t *value);
bool MOD_ReadMemoryBlock(uint32_t reloc_addr, uint8_t *data, size_t size);
bool MOD_GetActiveDelta(int64_t *delta);
bool MOD_ReadRuntimeMemoryU8(uint32_t linear_addr, uint8_t *value);
bool MOD_ReadRuntimeMemoryU16(uint32_t linear_addr, uint16_t *value);
bool MOD_ReadRuntimeMemoryU32(uint32_t linear_addr, uint32_t *value);
bool MOD_ReadRuntimeMemoryI32(uint32_t linear_addr, int32_t *value);
bool MOD_ReadRuntimeMemoryBlock(uint32_t linear_addr, uint8_t *data, size_t size);
bool MOD_WriteMemoryU8(uint32_t reloc_addr, uint8_t value);
bool MOD_WriteMemoryU16(uint32_t reloc_addr, uint16_t value);
bool MOD_WriteMemoryU32(uint32_t reloc_addr, uint32_t value);
bool MOD_WriteMemoryI32(uint32_t reloc_addr, int32_t value);

#endif
