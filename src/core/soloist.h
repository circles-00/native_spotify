#ifndef NATIVE_SOLOIST_H
#define NATIVE_SOLOIST_H

#include "core.h"
#include "engine.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Soloist's local WebSocket (--ws), used for playback on this device the way Spotifast uses it:
 * one long-lived connection only listens for change events, and every command or state read
 * opens its own short connection so a slow reply never stalls the events.
 */
void soloist_attach(unsigned port, void (*changed)(void));
void soloist_detach(void);

/* Unavailable means Soloist is not reachable or not the active device; use the Web API instead. */
EngineCommand soloist_command(const char *command, unsigned value);
bool soloist_playback(const char *device_id, const char *device_name, Playback *out);

/* Converts a Soloist playback_state message; now_ms is wall-clock time, as in Soloist's anchors. */
bool soloist_state_parse(const char *json, const char *device_id, const char *device_name, uint64_t now_ms,
                         Playback *out);

#endif
