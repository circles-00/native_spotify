/* Platform services that are identical on every POSIX system (Linux, Android). */
#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t platform_seconds(void) {
    return platform_milliseconds() / 1000;
}

uint64_t platform_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

uint64_t platform_unix_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

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

bool platform_make_dir(const char *path) {
    char partial[PLATFORM_PATH_CAPACITY];

    if (!path[0] || strlen(path) >= sizeof(partial)) {
        return false;
    }

    snprintf(partial, sizeof(partial), "%s", path);

    // Create missing parents (e.g. ~/.local/share on a fresh account); ours stay private.
    for (char *p = partial + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = 0;

        if (mkdir(partial, 0700) && errno != EEXIST) {
            return false;
        }

        *p = '/';
    }

    if (mkdir(partial, 0700) && errno != EEXIST) {
        return false;
    }

    struct stat info;
    return !stat(partial, &info) && S_ISDIR(info.st_mode);
}

bool platform_file_exists(const char *path) {
    struct stat info;
    return !stat(path, &info) && S_ISREG(info.st_mode);
}

bool platform_read_file(const char *path, char *out, size_t capacity) {
    int file = open(path, O_RDONLY | O_CLOEXEC);

    if (file < 0) {
        return false;
    }

    ssize_t size = read(file, out, capacity - 1);
    close(file);

    if (size < 0) {
        return false;
    }

    out[size] = 0;
    return true;
}

bool platform_write_file(const char *path, const char *text) {
    int file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (file < 0) {
        return false;
    }

    size_t length = strlen(text);
    bool ok = write(file, text, length) == (ssize_t)length;

    return close(file) == 0 && ok;
}
