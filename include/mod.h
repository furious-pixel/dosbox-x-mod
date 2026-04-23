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

struct ModPythonHookRegistration {
	size_t hook_id = 0;
	std::string exe_name_upper = {};
	uint32_t reloc_eip = 0;
	std::string kind = {};
	std::string description = {};
};

bool MOD_Init(const Config& config);
void MOD_Shutdown(void);
void MOD_OnOpenFile(const char *name, unsigned short handle);
void MOD_OnCloseFile(unsigned short handle);
bool MOD_FastEnabled(void);
void MOD_OnCallsite(uint32_t linear_eip);

bool MOD_ReadMemoryU8(uint32_t reloc_addr, uint8_t *value);
bool MOD_ReadMemoryU16(uint32_t reloc_addr, uint16_t *value);
bool MOD_ReadMemoryU32(uint32_t reloc_addr, uint32_t *value);
bool MOD_ReadMemoryI32(uint32_t reloc_addr, int32_t *value);
bool MOD_WriteMemoryU8(uint32_t reloc_addr, uint8_t value);
bool MOD_WriteMemoryU16(uint32_t reloc_addr, uint16_t value);
bool MOD_WriteMemoryU32(uint32_t reloc_addr, uint32_t value);
bool MOD_WriteMemoryI32(uint32_t reloc_addr, int32_t value);

#endif
