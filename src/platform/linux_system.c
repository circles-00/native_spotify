#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <errno.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
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

// Secrets are stored as plain text files under the user's cache directory (mode 0600 in a 0700
// directory). This is a deliberate choice so Linux works without a Secret Service provider.
static bool secret_path(const char *account, char *out, size_t capacity) {
    if (!account[0]) {
        return false;
    }

    for (const char *c = account; *c; ++c) {
        bool safe = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                    *c == '-' || *c == '_';

        if (!safe) {
            return false;
        }
    }

    const char *base = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    char directory[PLATFORM_PATH_CAPACITY];
    int size = 0;

    if (base && base[0] == '/') {
        size = snprintf(directory, sizeof(directory), "%s/native-spotify/credentials", base);
    } else if (home && home[0] == '/') {
        size = snprintf(directory, sizeof(directory), "%s/.cache/native-spotify/credentials", home);
    }

    if (size <= 0 || (size_t)size >= sizeof(directory)) {
        return false;
    }

    if (!platform_make_dir(directory)) {
        return false;
    }

    size = snprintf(out, capacity, "%s/%s", directory, account);

    return size > 0 && (size_t)size < capacity;
}

bool platform_secret_load(const char *account, char *out, size_t capacity) {
    char path[PLATFORM_PATH_CAPACITY];

    if (capacity == 0 || !secret_path(account, path, sizeof(path))) {
        return false;
    }

    int file = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (file < 0) {
        return false;
    }

    struct stat info;
    bool ok =
        !fstat(file, &info) && S_ISREG(info.st_mode) && info.st_size > 0 && (size_t)info.st_size < capacity;

    if (ok) {
        ssize_t size = read(file, out, (size_t)info.st_size);
        ok = size == (ssize_t)info.st_size && !memchr(out, 0, (size_t)size);

        if (ok) {
            out[size] = 0;
        } else {
            platform_wipe(out, capacity);
        }
    }

    close(file);

    return ok;
}

bool platform_secret_save(const char *account, const char *secret) {
    char path[PLATFORM_PATH_CAPACITY];
    char temporary[PLATFORM_PATH_CAPACITY + 8];

    if (!secret_path(account, path, sizeof(path))) {
        return false;
    }

    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    unlink(temporary);

    int file = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);

    if (file < 0) {
        return false;
    }

    size_t length = strlen(secret);
    bool ok = fchmod(file, 0600) == 0;

    if (ok) {
        ok = write(file, secret, length) == (ssize_t)length;
    }

    if (ok) {
        ok = fsync(file) == 0;
    }

    ok = close(file) == 0 && ok;

    if (ok) {
        ok = rename(temporary, path) == 0;
    }

    if (!ok) {
        unlink(temporary);
    }

    return ok;
}

bool platform_secret_delete(const char *account) {
    char path[PLATFORM_PATH_CAPACITY];

    if (!secret_path(account, path, sizeof(path))) {
        return false;
    }

    return unlink(path) == 0 || errno == ENOENT;
}
