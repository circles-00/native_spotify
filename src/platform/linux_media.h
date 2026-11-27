#ifndef NATIVE_LINUX_MEDIA_H
#define NATIVE_LINUX_MEDIA_H

#include "app.h"

#include <stdbool.h>

/*
 * Linux system media controls via MPRIS2 (org.mpris.MediaPlayer2) over the D-Bus session bus.
 * Runs an event-driven GLib main loop on a background thread with zero idle polling.
 */
bool linux_media_start(void);
bool linux_media_active(void);
void linux_media_update(const View *view);
bool linux_media_take_raise(void);
void linux_media_stop(void);

#endif
