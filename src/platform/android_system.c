/* Android platform adapter. POSIX services live in posix_common.c; this file covers the rest. */
#include "android_host.h"
#include "artwork.h"
#include "engine.h"
#include "platform.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JavaVM *java;
static jclass host;
static jmethodID host_http;
static jmethodID host_open_url;
static jmethodID host_secret_load;
static jmethodID host_secret_save;
static jmethodID host_secret_delete;
static jmethodID host_sha256;
static jmethodID host_wake;
static jmethodID host_engine_start;
static jmethodID host_engine_stop;
static pthread_key_t attached;
static char data_dir[PLATFORM_PATH_CAPACITY];

static void detach(void *value) {
    (void)value;

    if (java) {
        (*java)->DetachCurrentThread(java);
    }
}

bool android_host_load(JavaVM *vm, JNIEnv *env) {
    // Background threads get the system class loader, so look the class up here, on a Java thread.
    jclass type = (*env)->FindClass(env, "app/nativespotify/CoreHost");

    if (!type) {
        return false;
    }

    host = (*env)->NewGlobalRef(env, type);
    (*env)->DeleteLocalRef(env, type);
    host_http = (*env)->GetStaticMethodID(env, host, "http", "(Ljava/lang/String;Ljava/lang/String;[B[BZ)[B");
    host_open_url = (*env)->GetStaticMethodID(env, host, "openUrl", "(Ljava/lang/String;)Z");
    host_secret_load = (*env)->GetStaticMethodID(env, host, "secretLoad", "(Ljava/lang/String;)[B");
    host_secret_save = (*env)->GetStaticMethodID(env, host, "secretSave", "(Ljava/lang/String;[B)Z");
    host_secret_delete = (*env)->GetStaticMethodID(env, host, "secretDelete", "(Ljava/lang/String;)Z");
    host_sha256 = (*env)->GetStaticMethodID(env, host, "sha256", "([B)[B");
    host_wake = (*env)->GetStaticMethodID(env, host, "wake", "()V");
    host_engine_start = (*env)->GetStaticMethodID(env, host, "engineStart", "(I[BI)Z");
    host_engine_stop = (*env)->GetStaticMethodID(env, host, "engineStop", "(I)V");

    if (!host_http || !host_open_url || !host_secret_load || !host_secret_save || !host_secret_delete ||
        !host_sha256 || !host_wake || !host_engine_start || !host_engine_stop) {
        return false;
    }

    java = vm;
    return pthread_key_create(&attached, detach) == 0;
}

JNIEnv *android_env(void) {
    JNIEnv *env = NULL;

    if (!java) {
        return NULL;
    }

    jint state = (*java)->GetEnv(java, (void **)&env, JNI_VERSION_1_6);

    if (state == JNI_OK) {
        return env;
    }

    if (state != JNI_EDETACHED || (*java)->AttachCurrentThread(java, &env, NULL) != JNI_OK) {
        return NULL;
    }

    // Detach when this native thread exits; the value only needs to be non-null.
    pthread_setspecific(attached, env);
    return env;
}

/* Clears a pending Java exception so it cannot leak into later JNI calls. */
static bool failed(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) {
        return false;
    }

    (*env)->ExceptionClear(env);
    return true;
}

static jbyteArray bytes(JNIEnv *env, const char *text) {
    if (!text) {
        return NULL;
    }

    jsize size = (jsize)strlen(text);
    jbyteArray array = (*env)->NewByteArray(env, size);

    if (array) {
        (*env)->SetByteArrayRegion(env, array, 0, size, (const jbyte *)text);
    }

    return array;
}

/* Overwrites a Java byte array that held a token or secret before releasing it. */
static void release_secret(JNIEnv *env, jbyteArray array) {
    if (!array) {
        return;
    }

    jsize size = (*env)->GetArrayLength(env, array);
    jbyte *data = (*env)->GetByteArrayElements(env, array, NULL);

    if (data) {
        platform_wipe(data, (size_t)size);
        (*env)->ReleaseByteArrayElements(env, array, data, 0);
    }

    (*env)->DeleteLocalRef(env, array);
}

bool android_set_data_dir(const char *path) {
    int size = snprintf(data_dir, sizeof(data_dir), "%s", path);
    return size > 0 && (size_t)size < sizeof(data_dir) && platform_make_dir(data_dir);
}

void android_wake(void) {
    JNIEnv *env = android_env();

    if (!env) {
        return;
    }

    (*env)->CallStaticVoidMethod(env, host, host_wake);
    failed(env);
}

bool platform_init(void) {
    return java != NULL;
}

void platform_trim_memory(void) {
}

bool platform_random(unsigned char *out, size_t size) {
    arc4random_buf(out, size);
    return true;
}

bool platform_sha256(const char *text, unsigned char out[32]) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jbyteArray input = bytes(env, text);
    jbyteArray digest = input ? (*env)->CallStaticObjectMethod(env, host, host_sha256, input) : NULL;
    bool ok = !failed(env) && digest && (*env)->GetArrayLength(env, digest) == 32;

    if (ok) {
        (*env)->GetByteArrayRegion(env, digest, 0, 32, (jbyte *)out);
    }

    release_secret(env, input);

    if (digest) {
        (*env)->DeleteLocalRef(env, digest);
    }

    return ok;
}

/*
 * The host returns [status: 4 bytes][retry-after seconds: 4 bytes][body], big-endian,
 * or null when the request could not be made at all.
 */
bool platform_http(const char *method, const char *url, const char *token, const char *body, bool form,
                   HttpResponse *response) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jstring java_method = (*env)->NewStringUTF(env, method);
    jstring java_url = (*env)->NewStringUTF(env, url);
    jbyteArray java_token = bytes(env, token);
    jbyteArray java_body = bytes(env, body);
    jbyteArray result = NULL;

    if (java_method && java_url) {
        result = (*env)->CallStaticObjectMethod(env, host, host_http, java_method, java_url, java_token,
                                                java_body, (jboolean)form);
    }

    bool ok = !failed(env) && result;
    jsize size = ok ? (*env)->GetArrayLength(env, result) : 0;
    ok = ok && size >= 8 && (size_t)(size - 8) <= HTTP_BODY_LIMIT;
    char *data = ok ? malloc((size_t)size - 8 + 1) : NULL;

    if (ok && data) {
        unsigned char header[8];
        (*env)->GetByteArrayRegion(env, result, 0, 8, (jbyte *)header);
        (*env)->GetByteArrayRegion(env, result, 8, size - 8, (jbyte *)data);
        data[size - 8] = 0;

        response->status = (int)((uint32_t)header[0] << 24 | (uint32_t)header[1] << 16 |
                                 (uint32_t)header[2] << 8 | header[3]);
        uint32_t retry =
            (uint32_t)header[4] << 24 | (uint32_t)header[5] << 16 | (uint32_t)header[6] << 8 | header[7];
        response->retry_after = retry > 86400 ? 86400 : retry;
        response->body = data;
        response->size = (size_t)size - 8;
    }

    // Token responses carry credentials; never leave them in the Java heap copy.
    release_secret(env, result);
    release_secret(env, java_token);
    release_secret(env, java_body);

    if (java_method) {
        (*env)->DeleteLocalRef(env, java_method);
    }

    if (java_url) {
        (*env)->DeleteLocalRef(env, java_url);
    }

    return ok && data;
}

bool platform_open_url(const char *url) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jstring java_url = (*env)->NewStringUTF(env, url);
    bool ok = java_url && (*env)->CallStaticBooleanMethod(env, host, host_open_url, java_url);
    ok = !failed(env) && ok;

    if (java_url) {
        (*env)->DeleteLocalRef(env, java_url);
    }

    return ok;
}

bool platform_secret_load(const char *account, char *out, size_t capacity) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jstring name = (*env)->NewStringUTF(env, account);
    jbyteArray secret = name ? (*env)->CallStaticObjectMethod(env, host, host_secret_load, name) : NULL;
    bool ok = !failed(env) && secret;
    jsize size = ok ? (*env)->GetArrayLength(env, secret) : 0;
    ok = ok && (size_t)size < capacity;

    if (ok) {
        (*env)->GetByteArrayRegion(env, secret, 0, size, (jbyte *)out);
        out[size] = 0;
    }

    release_secret(env, secret);

    if (name) {
        (*env)->DeleteLocalRef(env, name);
    }

    return ok;
}

bool platform_secret_save(const char *account, const char *secret) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jstring name = (*env)->NewStringUTF(env, account);
    jbyteArray value = bytes(env, secret);
    bool ok = name && value && (*env)->CallStaticBooleanMethod(env, host, host_secret_save, name, value);
    ok = !failed(env) && ok;
    release_secret(env, value);

    if (name) {
        (*env)->DeleteLocalRef(env, name);
    }

    return ok;
}

bool platform_secret_delete(const char *account) {
    JNIEnv *env = android_env();

    if (!env) {
        return false;
    }

    jstring name = (*env)->NewStringUTF(env, account);
    bool ok = name && (*env)->CallStaticBooleanMethod(env, host, host_secret_delete, name);
    ok = !failed(env) && ok;

    if (name) {
        (*env)->DeleteLocalRef(env, name);
    }

    return ok;
}

bool platform_data_dir(char *out, size_t capacity) {
    int size = snprintf(out, capacity, "%s", data_dir);
    return data_dir[0] && size > 0 && (size_t)size < capacity;
}

/* Soloist runs in Termux on Android, never as a child of this app, and artwork loads in Compose. */
bool platform_download(const char *url, const char *path, size_t limit) {
    (void)url;
    (void)path;
    (void)limit;
    return false;
}

bool platform_extract(const char *archive, const char *directory) {
    (void)archive;
    (void)directory;
    return false;
}

/*
 * Soloist runs in Termux, started by the app's playback service. Each start gets an id so a
 * late exit or stop from an earlier session never affects the current one.
 */
struct PlatformProcess {
    int id;
    ProcessExit exited;
    void *context;
};

static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static PlatformProcess *engine;
static int engine_ids;

typedef struct {
    ProcessExit exited;
    void *context;
    int code;
} EngineExit;

static void report_exit(void *argument) {
    EngineExit report = *(EngineExit *)argument;
    free(argument);
    report.exited(report.code, report.context);
}

bool android_dispatch_local_playback(const LocalPlaybackCommand *command) {
    JNIEnv *env = android_env();

    if (!env || !host || !command) {
        return false;
    }

    jmethodID dispatch = (*env)->GetStaticMethodID(env, host, "dispatchLocalPlayback",
                                                  "(ILjava/lang/String;JJJ)Z");

    if (failed(env) || !dispatch) {
        return false;
    }

    jstring uri = command->uri ? (*env)->NewStringUTF(env, command->uri) : NULL;

    if (failed(env) || (command->uri && !uri)) {
        return false;
    }

    bool accepted = (*env)->CallStaticBooleanMethod(env, host, dispatch, (jint)command->action, uri,
                                                     (jlong)command->generation, (jlong)command->request_id,
                                                     (jlong)command->position_ms);
    (*env)->DeleteLocalRef(env, uri);
    return !failed(env) && accepted;
}

PlatformProcess *platform_process_start(const char *path, const char *const *arguments, ProcessExit exited,
                                        void *context) {
    (void)path;
    const char *key = NULL;
    unsigned socket_port = 0;

    // Termux starts Soloist; it needs the key and the loopback port the core will connect to.
    for (unsigned i = 0; arguments[i] && arguments[i + 1]; ++i) {
        if (!strcmp(arguments[i], "--api-key")) {
            key = arguments[i + 1];
        }

        if (!strcmp(arguments[i], "--ws")) {
            sscanf(arguments[i + 1], "127.0.0.1:%u", &socket_port);
        }
    }

    JNIEnv *env = android_env();
    PlatformProcess *process = key && env ? calloc(1, sizeof(*process)) : NULL;

    if (!process) {
        return NULL;
    }

    pthread_mutex_lock(&engine_lock);
    process->id = ++engine_ids;
    process->exited = exited;
    process->context = context;
    engine = process;
    pthread_mutex_unlock(&engine_lock);

    jbyteArray secret = bytes(env, key);
    bool started =
        secret && (*env)->CallStaticBooleanMethod(env, host, host_engine_start, process->id, secret,
                                                  (jint)socket_port);
    started = !failed(env) && started;
    release_secret(env, secret);

    if (!started) {
        pthread_mutex_lock(&engine_lock);
        engine = engine == process ? NULL : engine;
        pthread_mutex_unlock(&engine_lock);
        free(process);
        return NULL;
    }

    return process;
}

void android_engine_exited(int id, int code) {
    pthread_mutex_lock(&engine_lock);
    PlatformProcess *process = engine && engine->id == id ? engine : NULL;
    EngineExit *report = process ? malloc(sizeof(*report)) : NULL;

    // The handle stays owned by the core, which stops (frees) it after this report.
    if (report) {
        *report = (EngineExit){process->exited, process->context, code};
        engine = NULL;
    }

    pthread_mutex_unlock(&engine_lock);

    if (report && !platform_start_thread(report_exit, report)) {
        free(report);
    }
}

void platform_process_stop(PlatformProcess *process) {
    if (!process) {
        return;
    }

    pthread_mutex_lock(&engine_lock);
    bool running = engine == process;

    if (running) {
        engine = NULL;
    }

    pthread_mutex_unlock(&engine_lock);
    JNIEnv *env = running ? android_env() : NULL;

    if (env) {
        (*env)->CallStaticVoidMethod(env, host, host_engine_stop, process->id);
        failed(env);
    }

    free(process);
}

bool platform_image_decode(const void *data, size_t size, Image *image) {
    (void)data;
    (void)size;
    (void)image;
    return false;
}
