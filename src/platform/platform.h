#ifndef NATIVE_PLATFORM_H
#define NATIVE_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTP_BODY_LIMIT (1024 * 1024)
typedef struct {
    int status;
    unsigned retry_after;
    char *body;
    size_t size;
} HttpResponse;

bool platform_init(void);
bool platform_random(unsigned char *out, size_t size);
bool platform_sha256(const char *text, unsigned char out[32]);
uint64_t platform_seconds(void);
uint64_t platform_milliseconds(void);
void platform_sleep(unsigned milliseconds);
void platform_wipe(void *data, size_t size);
bool platform_http(const char *method, const char *url, const char *token, const char *body, bool form,
                   HttpResponse *response);
bool platform_open_url(const char *url);
bool platform_secret_load(const char *account, char *out, size_t capacity);
bool platform_secret_save(const char *account, const char *secret);
bool platform_secret_delete(const char *account);
bool platform_start_thread(void (*function)(void *), void *argument);
void platform_lock(void);
void platform_unlock(void);

/* Separate from the view lock: token refreshes do network I/O and must never stall drawing. */
void platform_token_lock(void);
void platform_token_unlock(void);

/* Paths are UTF-8. The data directory is per user and private to the current account. */
#define PLATFORM_PATH_CAPACITY 1024
bool platform_data_dir(char *out, size_t capacity);
bool platform_make_dir(const char *path);
bool platform_file_exists(const char *path);
bool platform_read_file(const char *path, char *out, size_t capacity);
bool platform_write_file(const char *path, const char *text);
bool platform_download(const char *url, const char *path, size_t limit);
bool platform_extract(const char *archive, const char *directory);

typedef struct PlatformProcess PlatformProcess;
typedef void (*ProcessExit)(int code, void *context);
PlatformProcess *platform_process_start(const char *path, const char *const *arguments, ProcessExit exited,
                                        void *context);
void platform_process_stop(PlatformProcess *process);

typedef intptr_t Listener;
#define INVALID_LISTENER ((Listener) - 1)
Listener loopback_open(void);
bool loopback_authorize(Listener listener, const char *state, char *code, size_t capacity);
void loopback_close(Listener listener);
#endif
