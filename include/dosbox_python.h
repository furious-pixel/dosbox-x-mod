#ifndef DOSBOX_PYTHON_H
#define DOSBOX_PYTHON_H

class Config;

bool DOSBoxPython_Init(const Config& config);
void DOSBoxPython_Shutdown(void);

#endif
