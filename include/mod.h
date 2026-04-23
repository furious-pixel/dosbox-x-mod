#ifndef DOSBOX_MOD_H
#define DOSBOX_MOD_H

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
};

bool MOD_Init(const Config& config);
void MOD_Shutdown(void);
void MOD_OnOpenFile(const char *name, unsigned short handle);
void MOD_OnCloseFile(unsigned short handle);

#endif
