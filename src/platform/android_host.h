#ifndef NATIVE_ANDROID_HOST_H
#define NATIVE_ANDROID_HOST_H

#include <jni.h>
#include <stdbool.h>

/*
 * Android platform services that need Java APIs (HTTPS, Keystore, browser) call static
 * methods on the Kotlin CoreHost object. These hooks are set once from JNI_OnLoad.
 */
bool android_host_load(JavaVM *vm, JNIEnv *env);

/* The calling thread's JNIEnv; core worker threads are attached on first use. */
JNIEnv *android_env(void);

/* App-private directory for core files (settings, history, saved playback). */
bool android_set_data_dir(const char *path);

/* Tells Kotlin that core state changed; called by app_wake. */
void android_wake(void);

/* Kotlin reports that the Termux engine session with this id ended. */
void android_engine_exited(int id, int code);


#endif
