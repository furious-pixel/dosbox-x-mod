#ifndef DOSBOX_PYTHON_H
#define DOSBOX_PYTHON_H

#include <vector>

#include "mod.h"

class Config;

bool DOSBoxPython_Init(const Config& config);
void DOSBoxPython_Shutdown(void);
bool DOSBoxPython_LoadModInitConfigs(std::vector<ModExecutableConfig> *configs);
bool DOSBoxPython_LoadMods(std::vector<ModPythonHookRegistration> *hooks);
void DOSBoxPython_ResetModRuntimeState(void);
void DOSBoxPython_ResetModStateTiming(void);
bool DOSBoxPython_UpdateModStateTiming(const ModFrameState &state);
bool DOSBoxPython_InvokeHook(size_t hook_id);
bool DOSBoxPython_InvokeSafePointCallback(void);
bool DOSBoxPython_OpenGLRendererAvailable(void);
const char *DOSBoxPython_GetOpenGLRendererSourceName(void);
void DOSBoxPython_NotifyOpenGLContextCreated(uint64_t context_generation);
void DOSBoxPython_NotifyOpenGLContextDestroying(void);
bool DOSBoxPython_InvokeOpenGLInitCallback(const ModOpenGLState &state);
bool DOSBoxPython_InvokeOpenGLCompositorCallback(const ModOpenGLState &state);

#endif
