#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <curl/curl.h>
#include <libsecret/secret.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <errno.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;
static const SecretSchema schema = {.name = "org.nativecontroller.Spotify",
                                    .flags = SECRET_SCHEMA_NONE,
                                    .attributes = {{"client-id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}};

bool platform_init(void) {
#ifdef __GLIBC__
    // Keep per-thread arenas and mmap thresholds bounded so multi-MB HTTP/JSON buffers return to the OS.
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif

    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

void platform_trim_memory(void) {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

bool platform_random(unsigned char *out, size_t size) {
    return size <= INT_MAX && RAND_bytes(out, (int)size) == 1;
}

bool platform_sha256(const char *text, unsigned char out[32]) {
    unsigned size = 0;

    return EVP_Digest(text, strlen(text), out, &size, EVP_sha256(), NULL) == 1 && size == 32;
}

uint64_t platform_seconds(void) {
    return platform_milliseconds() / 1000;
}

uint64_t platform_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

void platform_sleep(unsigned milliseconds) {
    struct timespec delay = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000};

    while (nanosleep(&delay, &delay) && errno == EINTR) {
    }
}

void platform_wipe(void *data, size_t size) {
    volatile unsigned char *p = data;

    while (size--) {
        *p++ = 0;
    }
}

typedef struct {
    char *data;
    size_t size;
    unsigned retry_after;
} Buffer;

static size_t receive_body(char *data, size_t size, size_t count, void *context) {
    Buffer *buffer = context;

    if (size && count > SIZE_MAX / size) {
        return 0;
    }

    size_t bytes = size * count;

    if (bytes > HTTP_BODY_LIMIT - buffer->size) {
        return 0;
    }

    char *grown = realloc(buffer->data, buffer->size + bytes + 1);

    if (!grown) {
        return 0;
    }

    buffer->data = grown;
    memcpy(buffer->data + buffer->size, data, bytes);
    buffer->size += bytes;
    buffer->data[buffer->size] = 0;

    return bytes;
}

static size_t receive_header(char *data, size_t size, size_t count, void *context) {
    Buffer *buffer = context;
    size_t bytes = size * count;

    if (bytes > 13 && !g_ascii_strncasecmp(data, "Retry-After:", 12)) {
        char number[32] = {0};
        size_t length = bytes - 12 < sizeof(number) - 1 ? bytes - 12 : sizeof(number) - 1;
        memcpy(number, data + 12, length);

        unsigned long seconds = strtoul(number, NULL, 10);
        buffer->retry_after = seconds > 86400 ? 86400 : (unsigned)seconds;
    }

    return bytes;
}

bool platform_http(const char *method, const char *url, const char *token, const char *body, bool form,
                   HttpResponse *response) {
    CURL *curl = curl_easy_init();

    if (!curl) {
        return false;
    }

    Buffer buffer = {0};
    struct curl_slist *headers = NULL;
    char authorization[4128] = {0};

    if (token) {
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", token);
        headers = curl_slist_append(headers, authorization);
    }

    if (form || body) {
        headers = curl_slist_append(headers, form ? "Content-Type: application/x-www-form-urlencoded"
                                                  : "Content-Type: application/json");
    } else if (!token) {
        headers = curl_slist_append(headers, "Accept: image/png, image/jpeg, image/webp;q=0.9, */*;q=0.8");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NativeController/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);

    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    platform_wipe(authorization, sizeof(authorization));

    if (result != CURLE_OK) {
        platform_wipe(buffer.data, buffer.size);
        free(buffer.data);

        return false;
    }

    response->status = (int)status;
    response->retry_after = buffer.retry_after;
    response->body = buffer.data ? buffer.data : calloc(1, 1);
    response->size = buffer.size;

    return response->body != NULL;
}

bool platform_open_url(const char *url) {
    GError *error = NULL;
    bool ok = g_app_info_launch_default_for_uri(url, NULL, &error);
    g_clear_error(&error);

    return ok;
}

bool platform_secret_load(const char *account, char *out, size_t capacity) {
    GError *error = NULL;
    char *secret = secret_password_lookup_sync(&schema, NULL, &error, "client-id", account, NULL);
    bool ok = !error && secret && strlen(secret) < capacity;

    if (ok) {
        memcpy(out, secret, strlen(secret) + 1);
    }

    if (secret) {
        platform_wipe(secret, strlen(secret));
    }

    secret_password_free(secret);
    g_clear_error(&error);

    return ok;
}

bool platform_secret_save(const char *account, const char *secret) {
    GError *error = NULL;

    bool ok =
        secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, "Native Controller Spotify login",
                                   secret, NULL, &error, "client-id", account, NULL);

    g_clear_error(&error);

    return ok;
}

bool platform_secret_delete(const char *account) {
    GError *error = NULL;
    secret_password_clear_sync(&schema, NULL, &error, "client-id", account, NULL);

    bool ok = !error;
    g_clear_error(&error);

    return ok;
}

typedef struct {
    void (*function)(void *);
    void *argument;
} Task;

static void *run_task(void *context) {
    Task task = *(Task *)context;
    free(context);

    task.function(task.argument);

    return NULL;
}

bool platform_start_thread(void (*function)(void *), void *argument) {
    Task *task = malloc(sizeof(*task));

    if (!task) {
        return false;
    }

    *task = (Task){function, argument};

    pthread_t thread;

    if (pthread_create(&thread, NULL, run_task, task)) {
        free(task);

        return false;
    }

    pthread_detach(thread);

    return true;
}

void platform_lock(void) {
    pthread_mutex_lock(&mutex);
}

void platform_unlock(void) {
    pthread_mutex_unlock(&mutex);
}

void platform_token_lock(void) {
    pthread_mutex_lock(&token_mutex);
}

void platform_token_unlock(void) {
    pthread_mutex_unlock(&token_mutex);
}
