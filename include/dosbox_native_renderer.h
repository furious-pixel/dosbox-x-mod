#ifndef DOSBOX_NATIVE_RENDERER_H
#define DOSBOX_NATIVE_RENDERER_H

#include <stdint.h>
#include <string>

#include "mod.h"

class Config;

bool DOSBoxNativeRenderer_Init(const Config& config);
void DOSBoxNativeRenderer_Shutdown(void);
bool DOSBoxNativeRenderer_Available(void);
const char *DOSBoxNativeRenderer_GetSourceName(void);
size_t DOSBoxNativeRenderer_GetHookCount(void);
bool DOSBoxNativeRenderer_GetHook(size_t index,
                                  std::string *exe_name_upper,
                                  uint32_t *event,
                                  uint32_t *kind,
                                  uint32_t *reloc_eip);
void DOSBoxNativeRenderer_NotifyOpenGLContextCreated(uint64_t generation);
void DOSBoxNativeRenderer_NotifyOpenGLContextLost(void);
bool DOSBoxNativeRenderer_InvokeOpenGLInit(const ModOpenGLState& state);
void DOSBoxNativeRenderer_OnFileOpened(const char *dos_name);
bool DOSBoxNativeRenderer_MissionBegin(int64_t delta);
void DOSBoxNativeRenderer_MissionEnd(void);
bool DOSBoxNativeRenderer_InvokeHook(uint32_t event,
                                     uint32_t kind,
                                     const ModFrameState& frame,
                                     int64_t delta);
void DOSBoxNativeRenderer_ServiceResources(void);
bool DOSBoxNativeRenderer_InvokeCompositor(const ModOpenGLState& state);
void DOSBoxNativeRenderer_GetLastTiming(double *extract_ms,
                                       double *draw_submit_ms);

#endif
