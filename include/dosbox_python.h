#ifndef DOSBOX_PYTHON_H
#define DOSBOX_PYTHON_H

class Config;

bool DOSBoxPython_Init(const Config& config);
void DOSBoxPython_Shutdown(void);
void DOSBoxPython_OnOpenFile(const char *name, unsigned short handle);
void DOSBoxPython_OnCloseFile(unsigned short handle);

#endif
