#define _GNU_SOURCE
#include "platform.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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

bool platform_data_dir(char *out, size_t capacity) {
    const char *base = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    int size = 0;

    if (base && base[0] == '/') {
        size = snprintf(out, capacity, "%s/native-spotify", base);
    }

    if (!(base && base[0] == '/') && home && home[0] == '/') {
        size = snprintf(out, capacity, "%s/.local/share/native-spotify", home);
    }

    return size > 0 && (size_t)size < capacity && platform_make_dir(out);
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

typedef struct {
    FILE *file;
    size_t written;
    size_t limit;
} Download;

static size_t receive_file(char *data, size_t size, size_t count, void *context) {
    Download *download = context;

    if (size && count > SIZE_MAX / size) {
        return 0;
    }

    size_t bytes = size * count;

    if (bytes > download->limit - download->written) {
        return 0;
    }

    download->written += bytes;
    return fwrite(data, 1, bytes, download->file);
}

bool platform_download(const char *url, const char *path, size_t limit) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (descriptor < 0) {
        return false;
    }

    Download download = {fdopen(descriptor, "wb"), 0, limit};
    CURL *curl = download.file ? curl_easy_init() : NULL;

    if (!curl) {
        if (download.file) {
            fclose(download.file);
        }
        if (!download.file) {
            close(descriptor);
        }

        unlink(path);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NativeController/0.1");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    bool ok = result == CURLE_OK && status == 200 && download.written > 0;
    ok = fclose(download.file) == 0 && ok;

    if (!ok) {
        unlink(path);
    }

    return ok;
}

static int run_and_wait(const char *path, char *const arguments[]) {
    posix_spawn_file_actions_t actions;

    if (posix_spawn_file_actions_init(&actions)) {
        return -1;
    }

    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t child;
    int result = posix_spawn(&child, path, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (result) {
        return -1;
    }

    int status = 0;

    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool platform_extract(const char *archive, const char *directory) {
    static const char *tools[] = {"/usr/bin/tar", "/bin/tar"};

    for (unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); ++i) {
        if (access(tools[i], X_OK)) {
            continue;
        }

        char *arguments[] = {(char *)tools[i], "-xzf", (char *)archive, "-C", (char *)directory, NULL};
        return run_and_wait(tools[i], arguments) == 0;
    }

    return false;
}

struct PlatformProcess {
    pid_t child;
    pthread_t watcher;
    pthread_mutex_t mutex;
    bool exited;
    bool stopping;
    ProcessExit exited_callback;
    void *context;
};

static void *watch_process(void *argument) {
    PlatformProcess *process = argument;
    int status = 0;

    while (waitpid(process->child, &status, 0) < 0 && errno == EINTR) {
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);

    pthread_mutex_lock(&process->mutex);
    process->exited = true;
    bool notify = !process->stopping;
    pthread_mutex_unlock(&process->mutex);

    if (notify) {
        process->exited_callback(code, process->context);
    }

    return NULL;
}

PlatformProcess *platform_process_start(const char *path, const char *const *arguments, ProcessExit exited,
                                        void *context) {
    PlatformProcess *process = calloc(1, sizeof(*process));

    if (!process) {
        return NULL;
    }

    pthread_mutex_init(&process->mutex, NULL);
    process->exited_callback = exited;
    process->context = context;
    pid_t parent = getpid();
    process->child = fork();

    if (process->child == 0) {
        // Only async-signal-safe calls between fork and exec. The child must not outlive us.
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        if (getppid() != parent) {
            _exit(127);
        }

        int quiet = open("/dev/null", O_RDWR);

        if (quiet >= 0) {
            dup2(quiet, STDIN_FILENO);
            dup2(quiet, STDOUT_FILENO);
            dup2(quiet, STDERR_FILENO);
        }

        close_range(3, ~0U, 0);
        execv(path, (char *const *)arguments);
        _exit(127);
    }

    if (process->child < 0 || pthread_create(&process->watcher, NULL, watch_process, process)) {
        if (process->child > 0) {
            kill(process->child, SIGKILL);
            waitpid(process->child, NULL, 0);
        }

        pthread_mutex_destroy(&process->mutex);
        free(process);
        return NULL;
    }

    return process;
}

void platform_process_stop(PlatformProcess *process) {
    if (!process) {
        return;
    }

    pthread_mutex_lock(&process->mutex);
    process->stopping = true;
    bool exited = process->exited;
    pthread_mutex_unlock(&process->mutex);

    if (!exited) {
        kill(process->child, SIGTERM);
    }

    uint64_t deadline = platform_milliseconds() + 3000;

    while (!exited && platform_milliseconds() < deadline) {
        platform_sleep(20);
        pthread_mutex_lock(&process->mutex);
        exited = process->exited;
        pthread_mutex_unlock(&process->mutex);
    }

    if (!exited) {
        kill(process->child, SIGKILL);
    }

    pthread_join(process->watcher, NULL);
    pthread_mutex_destroy(&process->mutex);
    free(process);
}
