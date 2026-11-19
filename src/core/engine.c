#include "engine.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define SEPARATOR "\\"
#define EXECUTABLE "soloist.exe"
#define ARCHIVE "soloist.zip"
#else
#define SEPARATOR "/"
#define EXECUTABLE "soloist"
#define ARCHIVE "soloist.tar.gz"
#endif

bool engine_key_normalize(const char *text, size_t size, char *out, size_t capacity) {
    // Clipboard text often carries surrounding whitespace or a trailing newline.
    while (size && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) {
        ++text;
        --size;
    }

    while (size && (text[size - 1] == ' ' || text[size - 1] == '\t' || text[size - 1] == '\r' ||
                    text[size - 1] == '\n' || !text[size - 1])) {
        --size;
    }

    if (size < 16 || size >= capacity || size >= ENGINE_KEY_CAPACITY) {
        return false;
    }

    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = (unsigned char)text[i];

        // The key is passed as one process argument; reject anything that is not a plain token.
        if (byte < 33 || byte > 126 || byte == '"' || byte == '\\') {
            return false;
        }
    }

    memcpy(out, text, size);
    out[size] = 0;
    return true;
}

static bool join(char *out, const char *base, const char *name) {
    int size = snprintf(out, PLATFORM_PATH_CAPACITY, "%s" SEPARATOR "%s", base, name);
    return size > 0 && size < PLATFORM_PATH_CAPACITY;
}

bool engine_paths(const char *base, EnginePaths *paths) {
    return join(paths->directory, base, "soloist") && join(paths->executable, paths->directory, EXECUTABLE) &&
           join(paths->archive, paths->directory, ARCHIVE) &&
           join(paths->stamp, paths->directory, "installed") && join(paths->data, paths->directory, "data") &&
           join(paths->cache, paths->directory, "cache");
}

const char *engine_archive_url(void) {
#ifdef _WIN32
    return "https://soloist-builds.spotifycdn.com/soloist_release_windows_x86_64.zip";
#elif defined(__aarch64__)
    return "https://soloist-builds.spotifycdn.com/soloist_release_arm64.tar.gz";
#elif defined(__arm__)
    return "https://soloist-builds.spotifycdn.com/soloist_release_arm32.tar.gz";
#else
    return "https://soloist-builds.spotifycdn.com/soloist_release_x86_64.tar.gz";
#endif
}

bool engine_needs_download(bool installed, const char *stamp, int64_t now_seconds) {
    if (!installed || !stamp) {
        return true;
    }

    char *end = NULL;
    long long installed_at = strtoll(stamp, &end, 10);

    // A missing, malformed or future stamp means we cannot trust the build's age.
    if (end == stamp || installed_at <= 0 || installed_at > now_seconds) {
        return true;
    }

    return now_seconds - installed_at >= ENGINE_REFRESH_SECONDS;
}

static EngineOptions options = {.device_name = ENGINE_DEVICE_NAME};

void engine_set_options(EngineOptions value) {
    options = value;

    if (!options.device_name || !options.device_name[0]) {
        options.device_name = ENGINE_DEVICE_NAME;
    }
}

const char *engine_device_name(void) {
    return options.device_name;
}

EngineCommand engine_command(const char *command, unsigned value) {
    return options.command ? options.command(command, value) : ENGINE_COMMAND_UNAVAILABLE;
}

bool engine_playback(const char *device, Playback *out) {
    return options.playback && options.playback(device, out);
}

bool engine_has_local_playback(void) {
    return options.playback != NULL;
}

bool engine_starts_on_play(void) {
    return options.start_on_play;
}

int engine_find_device(const Devices *devices) {
    for (unsigned i = 0; i < devices->count; ++i) {
        if (!strcmp(devices->items[i].name, options.device_name) && devices->items[i].id[0]) {
            return (int)i;
        }
    }

    return -1;
}

/* Runtime. Shared fields are guarded by platform_lock(); notify() is never called while holding it. */

static EngineNotify notify;
static EngineState state = ENGINE_OFF;
static PlatformProcess *process;
static unsigned generation;
static bool running;
static bool relaunch;
static bool force_download;
static unsigned restarts;
static uint64_t started_ms;

void engine_configure(EngineNotify callback) {
    notify = callback;
}

static bool publish(unsigned expected, EngineState next, const char *message) {
    platform_lock();
    bool current = expected == generation;

    if (current) {
        state = next;
    }

    platform_unlock();

    if (current && notify) {
        notify(next, message);
    }

    return current;
}

static void process_exited(int code, void *context);
static void launch(void);

/* Ends a run; a launch requested meanwhile (e.g. a new key) starts now with the current generation. */
static void finish(void) {
    platform_lock();
    running = false;
    bool again = relaunch && !process;
    relaunch = false;
    platform_unlock();

    if (again) {
        launch();
    }
}

static void stop_finished(void *finished) {
    platform_process_stop(finished);
}

/* Starts Soloist for the current generation and reports whether it is on its way. */
static void start(unsigned expected, const char *path, const char *const *arguments) {
    platform_lock();
    bool current = expected == generation;
    PlatformProcess *started =
        current ? platform_process_start(path, arguments, process_exited, (void *)(uintptr_t)expected) : NULL;

    if (started) {
        process = started;
        started_ms = platform_milliseconds();
    }

    platform_unlock();

    if (current && !started) {
        publish(expected, ENGINE_FAILED, "Could not start Spotify Soloist.");
    }

    if (started) {
        publish(expected, ENGINE_STARTING, "Starting Spotify Soloist…");
    }
}

static void run(void *argument) {
    unsigned expected = (unsigned)(uintptr_t)argument;
    char base[PLATFORM_PATH_CAPACITY];
    char key[ENGINE_KEY_CAPACITY] = {0};
    EnginePaths paths;

    if (!platform_secret_load(ENGINE_KEY_ACCOUNT, key, sizeof(key)) || !key[0]) {
        publish(expected, ENGINE_NEEDS_KEY, "");
        finish();
        return;
    }

    // A hosting platform installs Soloist itself; it only needs the key and the device name.
    if (options.hosted) {
        const char *arguments[] = {EXECUTABLE, "--device-name", options.device_name, "--api-key", key, NULL};
        start(expected, EXECUTABLE, arguments);
        platform_wipe(key, sizeof(key));
        finish();
        return;
    }

    bool ready = platform_data_dir(base, sizeof(base)) && engine_paths(base, &paths) &&
                 platform_make_dir(paths.directory) && platform_make_dir(paths.data) &&
                 platform_make_dir(paths.cache);
    char stamp[64] = {0};
    bool stamped = ready && platform_read_file(paths.stamp, stamp, sizeof(stamp));

    platform_lock();
    bool refresh = force_download;
    force_download = false;
    platform_unlock();

    if (ready && (refresh || engine_needs_download(platform_file_exists(paths.executable),
                                                   stamped ? stamp : NULL, (int64_t)time(NULL)))) {
        publish(expected, ENGINE_DOWNLOADING, "Downloading Spotify Soloist from Spotify…");

        // Spotify forbids redistributing Soloist, so each install fetches the official build itself.
        ready = platform_download(engine_archive_url(), paths.archive, ENGINE_DOWNLOAD_LIMIT) &&
                platform_extract(paths.archive, paths.directory) && platform_file_exists(paths.executable);

        if (ready) {
            snprintf(stamp, sizeof(stamp), "%" PRId64, (int64_t)time(NULL));
            platform_write_file(paths.stamp, stamp);
        }

        if (!ready) {
            platform_wipe(key, sizeof(key));
            publish(expected, ENGINE_FAILED,
                    "Could not download Spotify Soloist. Check your connection, then try again.");
            finish();
            return;
        }
    }

    if (!ready) {
        platform_wipe(key, sizeof(key));
        publish(expected, ENGINE_FAILED, "Could not create the Soloist folder in your user profile.");
        finish();
        return;
    }

    const char *arguments[] = {paths.executable, "--device-name", options.device_name, "--api-key", key,
                               "--data-dir",     paths.data,      "--cache-dir",       paths.cache, NULL};

    start(expected, paths.executable, arguments);
    platform_wipe(key, sizeof(key));
    finish();
}

static void launch(void) {
    platform_lock();
    bool start = !running && !process;
    unsigned expected = generation;

    if (start) {
        running = true;
    }

    if (running && !start) {
        relaunch = true;
    }

    platform_unlock();

    if (start && !platform_start_thread(run, (void *)(uintptr_t)expected)) {
        platform_lock();
        running = false;
        platform_unlock();
        publish(expected, ENGINE_FAILED, "Could not start Spotify Soloist.");
    }
}

static void process_exited(int code, void *context) {
    unsigned expected = (unsigned)(uintptr_t)context;

    platform_lock();
    bool current = expected == generation;
    PlatformProcess *finished = current ? process : NULL;
    bool early = platform_milliseconds() - started_ms < 15000;

    if (current) {
        process = NULL;
        force_download = code == 10;
        restarts = early ? restarts + 1 : 0;
    }

    unsigned attempts = restarts;
    platform_unlock();

    if (!current) {
        return;
    }

    // The watcher thread is finishing; release its handles from another thread.
    if (finished) {
        platform_start_thread(stop_finished, finished);
    }

    // Exit code 10 is Soloist's "build expired": fetch the current build and restart.
    if (code == 10 || attempts < 3) {
        launch();
        return;
    }

    char message[256];
    snprintf(
        message, sizeof(message),
        "Spotify Soloist keeps stopping (code %d). Check that your Soloist key is correct, then paste it "
        "again.",
        code);
    publish(expected, ENGINE_FAILED, message);
}

void engine_start(void) {
    launch();
}

bool engine_set_key(const char *text, size_t size) {
    char key[ENGINE_KEY_CAPACITY];

    if (!engine_key_normalize(text, size, key, sizeof(key))) {
        return false;
    }

    bool saved = platform_secret_save(ENGINE_KEY_ACCOUNT, key);
    platform_wipe(key, sizeof(key));

    if (!saved) {
        return false;
    }

    engine_stop();
    platform_lock();
    restarts = 0;
    platform_unlock();
    launch();
    return true;
}

void engine_device_seen(bool seen) {
    platform_lock();
    EngineState current = state;
    unsigned expected = generation;
    bool waited = platform_milliseconds() - started_ms > 8000;
    platform_unlock();

    if (seen && (current == ENGINE_STARTING || current == ENGINE_PAIRING)) {
        publish(expected, ENGINE_READY, "");
    }

    if (!seen && current == ENGINE_READY) {
        publish(expected, ENGINE_STARTING, "Reconnecting Spotify Soloist…");
    }

    // Soloist only logs in after it is picked once from an official Spotify app.
    if (!seen && current == ENGINE_STARTING && waited) {
        char message[256];
        snprintf(message, sizeof(message),
                 "One-time step: open Spotify on your phone, tap the devices icon and choose “%s”. If it "
                 "does not connect, check your Soloist key.",
                 options.device_name);
        publish(expected, ENGINE_PAIRING, message);
    }
}

void engine_stop(void) {
    platform_lock();
    ++generation;
    PlatformProcess *stopping = process;
    process = NULL;
    state = ENGINE_OFF;
    platform_unlock();

    platform_process_stop(stopping);
}

void engine_forget(void) {
    engine_stop();
    platform_secret_delete(ENGINE_KEY_ACCOUNT);
}
