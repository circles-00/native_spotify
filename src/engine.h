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

void engine_configure(EngineNotify notify);
void engine_start(void);
bool engine_set_key(const char *text, size_t size);
void engine_device_seen(bool seen);
void engine_stop(void);
void engine_forget(void);

#endif
