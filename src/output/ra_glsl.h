#ifndef DOSBOX_RA_GLSL_H
#define DOSBOX_RA_GLSL_H

#include "config.h"

#if C_OPENGL

#include <string>

bool RA_GLSL_FindPreset(const std::string &name, std::string &out_path);
bool RA_GLSL_CyclePreset(const std::string &current, int delta, std::string &out_name);
void RA_GLSL_SetPresetPath(const char *path);
bool RA_GLSL_HasPreset(void);
void RA_GLSL_Release(void);
bool RA_GLSL_Draw(unsigned int src_texture,
                  int src_tex_size,
                  int src_w,
                  int src_h,
                  int viewport_x,
                  int viewport_y,
                  int viewport_w,
                  int viewport_h,
                  int frame_count,
                  unsigned long long context_generation);

#endif /* C_OPENGL */

#endif /* DOSBOX_RA_GLSL_H */
