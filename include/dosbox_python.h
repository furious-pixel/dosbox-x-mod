#ifndef DOSBOX_PYTHON_H
#define DOSBOX_PYTHON_H

#include <vector>

#include "mod.h"

class Config;

bool DOSBoxPython_Init(const Config& config);
void DOSBoxPython_Shutdown(void);
bool DOSBoxPython_LoadModInitConfigs(std::vector<ModExecutableConfig> *configs);

#endif
