#ifndef NATIVE_ENGINE_H
#define NATIVE_ENGINE_H

#include "platform.h"
#include "player.h"

#include <stdbool.h>
#include <stdint.h>

/* Spotify Soloist: Spotify's official headless player, run as an owned background process. */

#define ENGINE_DEVICE_NAME "Native Spotify"
#define ENGINE_KEY_ACCOUNT "native-spotify-soloist-key"
#define ENGINE_KEY_CAPACITY 512
#define ENGINE_DASHBOARD_URL "https://developer.spotify.com/dashboard/soloist"
#define ENGINE_DOWNLOAD_LIMIT (256u * 1024u * 1024u)

/* Builds expire 90 days after their build date; refreshing monthly stays well inside that. */
#define ENGINE_REFRESH_SECONDS (30 * 24 * 60 * 60)

typedef enum {
    ENGINE_OFF,
    ENGINE_NEEDS_KEY,
    ENGINE_DOWNLOADING,
    ENGINE_STARTING,
    ENGINE_PAIRING,
    ENGINE_READY,
    ENGINE_FAILED
} EngineState;

typedef struct {
    char directory[PLATFORM_PATH_CAPACITY];
    char executable[PLATFORM_PATH_CAPACITY];
    char archive[PLATFORM_PATH_CAPACITY];
    char stamp[PLATFORM_PATH_CAPACITY];
    char data[PLATFORM_PATH_CAPACITY];
    char cache[PLATFORM_PATH_CAPACITY];
} EnginePaths;

bool engine_key_normalize(const char *text, size_t size, char *out, size_t capacity);
bool engine_paths(const char *base, EnginePaths *paths);
const char *engine_archive_url(void);
bool engine_needs_download(bool installed, const char *stamp, int64_t now_seconds);
int engine_find_device(const Devices *devices);

typedef void (*EngineNotify)(EngineState state, const char *message);

/*
 * How this platform runs Soloist. The defaults are the desktop's: the core downloads Soloist,
 * starts it with the session and finds it as ENGINE_DEVICE_NAME. A platform that installs and
 * hosts Soloist itself (Android, through Termux) sets these before app_initialize().
 */
typedef enum {
    ENGINE_COMMAND_UNAVAILABLE,
    ENGINE_COMMAND_ACCEPTED,
    ENGINE_COMMAND_UNCONFIRMED,
    ENGINE_COMMAND_REJECTED
} EngineCommand;

/* command and playback replace Soloist's local socket; only tests set them. */
typedef struct {
    const char *device_name;
    bool hosted;
    bool start_on_play;
    EngineCommand (*command)(const char *command, unsigned value);
    bool (*playback)(const char *device, Playback *out);
} EngineOptions;

/* Commands and state for this device's Soloist; unavailable means use the Web API. */
EngineCommand engine_command(const char *command, unsigned value);

typedef enum {
    LOCAL_PLAYBACK_PLAY,
    LOCAL_PLAYBACK_PAUSE,
    LOCAL_PLAYBACK_RESUME,
    LOCAL_PLAYBACK_SEEK,
    LOCAL_PLAYBACK_STOP
} LocalPlaybackAction;

typedef struct LocalPlaybackCommand {
    LocalPlaybackAction action;
    const char *uri;
    uint64_t generation;
    uint64_t request_id;
    int64_t position_ms;
} LocalPlaybackCommand;

typedef enum {
    LOCAL_PLAYBACK_BUFFERING,
    LOCAL_PLAYBACK_PLAYING,
    LOCAL_PLAYBACK_PAUSED,
    LOCAL_PLAYBACK_SEEKED,
    LOCAL_PLAYBACK_ENDED,
    LOCAL_PLAYBACK_ERROR
} LocalPlaybackState;

typedef struct {
    uint64_t generation;
    uint64_t request_id;
    LocalPlaybackState state;
    int64_t position_ms;
    int64_t duration_ms;
} LocalPlaybackEvent;

bool engine_dispatch_local_playback(const LocalPlaybackCommand *command);
#ifdef __ANDROID__
bool android_dispatch_local_playback(const LocalPlaybackCommand *command);
#endif
bool engine_playback(const char *device, Playback *out);
void engine_set_options(EngineOptions options);
const char *engine_device_name(void);
bool engine_starts_on_play(void);

void engine_configure(EngineNotify notify, void (*playback_changed)(void));
void engine_start(void);
bool engine_set_key(const char *text, size_t size);
void engine_device_seen(bool seen);
void engine_stop(void);
void engine_forget(void);

#endif
