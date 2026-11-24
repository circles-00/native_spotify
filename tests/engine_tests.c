#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include "platform.h"
#include "soloist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static unsigned checks;

#define CHECK(expression)                                                                                    \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(expression)) {                                                                                 \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression);                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void test_keys(void) {
    char key[ENGINE_KEY_CAPACITY];
    const char *pasted = "  \tsk_live_0123456789abcdefABCDEF\r\n";

    CHECK(engine_key_normalize(pasted, strlen(pasted), key, sizeof(key)));
    CHECK(!strcmp(key, "sk_live_0123456789abcdefABCDEF"));

    static const char *rejected[] = {"",
                                     "short",
                                     "has a space inside it",
                                     "quote\"inside0123456",
                                     "back\\slash01234567",
                                     "line\nbreak0123456789"};

    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        CHECK(!engine_key_normalize(rejected[i], strlen(rejected[i]), key, sizeof(key)));
    }

    char huge[ENGINE_KEY_CAPACITY + 8];
    memset(huge, 'k', sizeof(huge));
    CHECK(!engine_key_normalize(huge, sizeof(huge), key, sizeof(key)));
    CHECK(!engine_key_normalize(pasted, strlen(pasted), key, 8));
}

static void test_update_policy(void) {
    int64_t now = 1800000000;
    char stamp[32];

    CHECK(engine_needs_download(false, "1799999999", now));
    CHECK(engine_needs_download(true, NULL, now));
    CHECK(engine_needs_download(true, "garbage", now));
    CHECK(engine_needs_download(true, "0", now));

    snprintf(stamp, sizeof(stamp), "%lld", (long long)(now + 60));
    CHECK(engine_needs_download(true, stamp, now));

    snprintf(stamp, sizeof(stamp), "%lld", (long long)(now - ENGINE_REFRESH_SECONDS + 3600));
    CHECK(!engine_needs_download(true, stamp, now));

    snprintf(stamp, sizeof(stamp), "%lld", (long long)(now - ENGINE_REFRESH_SECONDS));
    CHECK(engine_needs_download(true, stamp, now));

    CHECK(!strncmp(engine_archive_url(), "https://soloist-builds.spotifycdn.com/soloist_release_", 54));
}

static void test_devices_and_paths(void) {
    Devices devices;
    devices_reset(&devices);
    devices.count = 3;
    snprintf(devices.items[0].name, sizeof(devices.items[0].name), "Kitchen");
    snprintf(devices.items[0].id, sizeof(devices.items[0].id), "a");
    snprintf(devices.items[1].name, sizeof(devices.items[1].name), "%s", ENGINE_DEVICE_NAME);
    snprintf(devices.items[2].name, sizeof(devices.items[2].name), "%s", ENGINE_DEVICE_NAME);
    snprintf(devices.items[2].id, sizeof(devices.items[2].id), "b");

    // A listed device without an ID cannot be targeted, so the next match wins.
    CHECK(engine_find_device(&devices) == 2);
    devices.count = 1;
    CHECK(engine_find_device(&devices) == -1);

    EnginePaths paths;
    CHECK(engine_paths("/base", &paths));
#ifdef _WIN32
    CHECK(!strcmp(paths.executable, "/base\\soloist\\soloist.exe"));
#else
    CHECK(!strcmp(paths.executable, "/base/soloist/soloist"));
    CHECK(!strcmp(paths.data, "/base/soloist/data"));
    CHECK(!strcmp(paths.archive, "/base/soloist/soloist.tar.gz"));
#endif

    char long_base[PLATFORM_PATH_CAPACITY];
    memset(long_base, 'x', sizeof(long_base) - 1);
    long_base[sizeof(long_base) - 1] = 0;
    CHECK(!engine_paths(long_base, &paths));
}

typedef struct {
    int code;
    unsigned calls;
} Exit;

static void exited(int code, void *context) {
    Exit *result = context;

    platform_lock();
    result->code = code;
    ++result->calls;
    platform_unlock();
}

static void test_processes(void) {
#ifdef _WIN32
    char shell[MAX_PATH];
    GetSystemDirectoryA(shell, MAX_PATH);
    strcat(shell, "\\cmd.exe");
    const char *expire[] = {shell, "/c", "exit 10", NULL};
    const char *wait[] = {shell, "/c", "ping -n 30 127.0.0.1", NULL};
#else
    const char *shell = "/bin/sh";
    const char *expire[] = {shell, "-c", "exit 10", NULL};
    const char *wait[] = {shell, "-c", "sleep 30", NULL};
#endif

    // Soloist exits with code 10 when its build expires; the engine relies on seeing that code.
    Exit result = {-1, 0};
    PlatformProcess *process = platform_process_start(shell, expire, exited, &result);
    CHECK(process != NULL);

    uint64_t deadline = platform_milliseconds() + 5000;
    unsigned calls = 0;

    while (!calls && platform_milliseconds() < deadline) {
        platform_sleep(10);
        platform_lock();
        calls = result.calls;
        platform_unlock();
    }

    CHECK(calls == 1 && result.code == 10);
    platform_process_stop(process);

    // Stopping an owned process ends it promptly and does not report an unexpected exit.
    result = (Exit){-1, 0};
    process = platform_process_start(shell, wait, exited, &result);
    CHECK(process != NULL);
    platform_sleep(100);
    uint64_t start = platform_milliseconds();
    platform_process_stop(process);
    CHECK(platform_milliseconds() - start < 4000);
    CHECK(result.calls == 0);

    const char *missing[] = {"/nonexistent/soloist", NULL};
    result = (Exit){-1, 0};
    process = platform_process_start("/nonexistent/soloist", missing, exited, &result);

    if (process) {
        platform_sleep(200);
        platform_process_stop(process);
    }

    CHECK(!process || result.calls <= 1);
}

static void test_files(void) {
#ifndef _WIN32
    char root[] = "/tmp/native-engine-test-XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char nested[512];
    char file[600];
    char text[64];
    snprintf(nested, sizeof(nested), "%s/a/b/c", root);
    snprintf(file, sizeof(file), "%s/stamp", nested);

    CHECK(platform_make_dir(nested));
    CHECK(!platform_file_exists(nested));
    CHECK(platform_write_file(file, "1800000000"));
    CHECK(platform_file_exists(file));
    CHECK(platform_read_file(file, text, sizeof(text)) && !strcmp(text, "1800000000"));

    // A real archive laid out like Soloist's release, extracted with the platform's own tool.
    char command[1024];
    char archive[512];
    char target[512];
    char extracted[600];
    snprintf(archive, sizeof(archive), "%s/soloist.tar.gz", root);
    snprintf(target, sizeof(target), "%s/out", root);
    snprintf(extracted, sizeof(extracted), "%s/soloist", target);
    snprintf(command, sizeof(command),
             "cd '%s' && printf binary > soloist && tar -czf soloist.tar.gz soloist", root);
    CHECK(system(command) == 0);
    CHECK(platform_make_dir(target));
    CHECK(platform_extract(archive, target));
    CHECK(platform_read_file(extracted, text, sizeof(text)) && !strcmp(text, "binary"));

    snprintf(command, sizeof(command), "printf broken > '%s'", archive);
    CHECK(system(command) == 0);
    CHECK(!platform_extract(archive, target));

    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    CHECK(system(command) == 0);
#endif
}

#ifndef _WIN32
/* A minimal stand-in for Soloist's socket: answers handshakes, get_state and commands. */
typedef struct {
    int listener;
    char last_command[256];
} FakeSoloist;

static bool read_all(int socket, unsigned char *out, size_t size) {
    while (size) {
        ssize_t count = recv(socket, out, size, 0);

        if (count <= 0) {
            return false;
        }

        out += count;
        size -= (size_t)count;
    }

    return true;
}

static void reply(int socket, const char *text) {
    size_t size = strlen(text);
    unsigned char head[4] = {0x81, 0, 0, 0};
    size_t header = 2;

    if (size < 126) {
        head[1] = (unsigned char)size;
    } else {
        head[1] = 126;
        head[2] = (unsigned char)(size >> 8);
        head[3] = (unsigned char)size;
        header = 4;
    }

    send(socket, head, header, 0);
    send(socket, text, size, 0);
}

static void serve(void *argument) {
    FakeSoloist *fake = argument;

    for (int client; (client = accept(fake->listener, NULL, NULL)) >= 0;) {
        char handshake[1024] = {0};
        size_t length = 0;

        while (length < sizeof(handshake) - 1 && !strstr(handshake, "\r\n\r\n") &&
               recv(client, handshake + length, 1, 0) == 1) {
            ++length;
        }

        const char *upgrade = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        send(client, upgrade, strlen(upgrade), 0);

        unsigned char head[2];
        unsigned char mask[4];
        char frame[256] = {0};

        if (!read_all(client, head, 2) || (head[1] & 0x7f) >= sizeof(frame) || !read_all(client, mask, 4) ||
            !read_all(client, (unsigned char *)frame, head[1] & 0x7f)) {
            close(client);
            continue;
        }

        for (size_t i = 0; i < (head[1] & 0x7fu); ++i) {
            frame[i] ^= (char)mask[i % 4];
        }

        // A pushed event first: clients must skip it while waiting for their reply.
        reply(client, "{\"type\":\"volume_changed\",\"volume\":50}");

        if (strstr(frame, "get_state")) {
            reply(client, "{\"type\":\"playback_state\",\"is_active\":true,\"status\":\"paused\","
                          "\"position\":{\"position_ms\":4200,\"timestamp_ms\":1,\"speed\":0.0},"
                          "\"item\":{\"uri\":\"spotify:track:4uLU6hMCjMI75M1A2tKUQC\"}}");
            continue;
        }

        snprintf(fake->last_command, sizeof(fake->last_command), "%s", frame);
        reply(client, strstr(frame, "skip_next") ? "{\"type\":\"command_result\",\"command\":\"skip_next\","
                                                   "\"success\":false}"
                                                 : "{\"type\":\"command_result\",\"command\":\"seek\"}");
        close(client);
    }
}

static unsigned changes;

static void changed(void) {
    ++changes;
}

static void test_soloist_socket(void) {
    FakeSoloist fake = {.listener = socket(AF_INET, SOCK_STREAM, 0)};
    unsigned port = loopback_free_port();
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(port && bind(fake.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(listen(fake.listener, 8) == 0);
    CHECK(platform_start_thread(serve, &fake));

    // Before the event connection learns this device is active, everything falls back to the Web API.
    CHECK(soloist_command("seek:spotify:track:x", 1000) == ENGINE_COMMAND_UNAVAILABLE);
    soloist_attach(port, changed);

    Playback playback;
    bool read = false;

    for (unsigned attempt = 0; attempt < 100 && !read; ++attempt) {
        read = soloist_playback("phone", "Native Spotify Phone", &playback);

        if (!read) {
            platform_sleep(20);
        }
    }

    CHECK(read && changes > 0);
    CHECK(!playback.playing && playback.position_ms == 4200);
    CHECK(soloist_command("seek:spotify:track:x", 91000) == ENGINE_COMMAND_ACCEPTED);
    CHECK(strstr(fake.last_command, "\"command\":\"seek\"") && strstr(fake.last_command, "\"position_ms\":91000"));
    CHECK(soloist_command("next", 0) == ENGINE_COMMAND_REJECTED);
    CHECK(soloist_command("enqueue:spotify:track:x", 0) == ENGINE_COMMAND_UNAVAILABLE);

    soloist_detach();
    CHECK(soloist_command("pause", 0) == ENGINE_COMMAND_UNAVAILABLE);
    shutdown(fake.listener, SHUT_RDWR);
    close(fake.listener);
}
#endif

int main(void) {
    CHECK(platform_init());
    test_keys();
    test_update_policy();
    test_devices_and_paths();
    test_processes();
    test_files();
#ifndef _WIN32
    test_soloist_socket();
#endif

    printf("PASS: %u engine checks (Soloist key handling, update policy, device match, owned processes, "
           "files, archive extraction)\n",
           checks);
    return 0;
}
