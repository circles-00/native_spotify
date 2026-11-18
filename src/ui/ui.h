#ifndef NATIVE_UI_H
#define NATIVE_UI_H

#include <stdbool.h>

int ui_run(bool smoke_test);
void ui_wake(void);

/* Reads the clipboard and hands it to app_engine_key(); asynchronous on X11. */
void ui_paste_engine_key(void);

#endif
