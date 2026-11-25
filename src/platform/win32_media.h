#ifndef NATIVE_WIN32_MEDIA_H
#define NATIVE_WIN32_MEDIA_H

#include "app.h"

#include <windows.h>

/*
 * System media controls: media keys, the Windows media flyout and the lock screen. Buttons post
 * button_message to the window with an Action in wParam. Call from the window's COM-initialized STA thread.
 */
bool win32_media_start(HWND window, UINT button_message);
bool win32_media_active(void);
void win32_media_update(const View *view);
void win32_media_stop(void);

#endif
