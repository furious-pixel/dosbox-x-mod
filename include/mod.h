#ifndef DOSBOX_MOD_H
#define DOSBOX_MOD_H

#include <stddef.h>
#include <stdint.h>
#include <string>

class Config;

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
};

struct ModFrameState {
	uint64_t frame = 0;
	double time_seconds = 0.0;
	double frame_delta_seconds = 0.0;
};

enum ModRenderViewMode {
	MOD_RENDER_VIEW_GAME_ONLY = 0,
	MOD_RENDER_VIEW_MOD_ONLY = 1,
	MOD_RENDER_VIEW_SIDE_BY_SIDE = 2,
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
void MOD_OnCallsite(uint32_t linear_eip);

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
