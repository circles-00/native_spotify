#include "platform.h"

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <wincred.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SRWLOCK mutex = SRWLOCK_INIT;
static SRWLOCK token_mutex = SRWLOCK_INIT;

bool platform_init(void) {
    WSADATA data;

    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

bool platform_random(unsigned char *out, size_t size) {
    return size <= ULONG_MAX && BCryptGenRandom(NULL, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool platform_sha256(const char *text, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0)) {
        return false;
    }

    NTSTATUS result = BCryptHash(algorithm, NULL, 0, (PUCHAR)text, (ULONG)strlen(text), out, 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    return result == 0;
}

uint64_t platform_seconds(void) {
    return GetTickCount64() / 1000;
}

uint64_t platform_milliseconds(void) {
    return GetTickCount64();
}

void platform_sleep(unsigned milliseconds) {
    Sleep(milliseconds);
}

void platform_wipe(void *data, size_t size) {
    if (data) {
        SecureZeroMemory(data, size);
    }
}

static bool wide(const char *text, wchar_t *out, int capacity) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, capacity) > 0;
}

bool platform_http(const char *method, const char *url, const char *token, const char *body, bool form,
                   HttpResponse *response) {
    wchar_t address[2048];
    wchar_t verb[16];
    wchar_t host[256];
    wchar_t path[2048];
    wchar_t headers[4352] = {0};
    char authorization[4352] = {0};

    if (!wide(url, address, 2048) || !wide(method, verb, 16)) {
        return false;
    }

    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(address, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"NativeController/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session) {
        return false;
    }

    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, verb, path, NULL, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : NULL;

    bool ok = false;
    char *data = NULL;
    size_t length = 0;

    if (!request) {
        goto done;
    }

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;

    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) {
        goto done;
    }

    snprintf(authorization, sizeof(authorization), "%s%s%sContent-Type: %s\r\n",
             token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "",
             form ? "application/x-www-form-urlencoded" : "application/json");

    if (!wide(authorization, headers, 4352)) {
        goto done;
    }

    if (!WinHttpSendRequest(request, headers, (DWORD)-1, (void *)body, body ? (DWORD)strlen(body) : 0,
                            body ? (DWORD)strlen(body) : 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) {
        goto done;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);

    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        goto done;
    }

    response->status = (int)status;

    wchar_t retry[32] = {0};
    size = sizeof(retry);

    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        unsigned long seconds = wcstoul(retry, NULL, 10);
        response->retry_after = seconds > 86400 ? 86400 : (unsigned)seconds;
    }

    data = calloc(1, 1);

    if (!data) {
        goto done;
    }

    for (;;) {
        DWORD available = 0;

        if (!WinHttpQueryDataAvailable(request, &available)) {
            goto done;
        }

        if (!available) {
            break;
        }

        if (available > HTTP_BODY_LIMIT - length) {
            goto done;
        }

        char *grown = realloc(data, length + available + 1);

        if (!grown) {
            goto done;
        }

        data = grown;
        DWORD received = 0;

        if (!WinHttpReadData(request, data + length, available, &received)) {
            goto done;
        }

        if (!received) {
            break;
        }

        length += received;
        data[length] = 0;
    }

    response->body = data;
    response->size = length;
    ok = true;

done:
    if (!ok) {
        platform_wipe(data, length);
        free(data);
    }

    platform_wipe(authorization, sizeof(authorization));
    platform_wipe(headers, sizeof(headers));

    if (request) {
        WinHttpCloseHandle(request);
    }

    if (connection) {
        WinHttpCloseHandle(connection);
    }

    WinHttpCloseHandle(session);

    return ok;
}

bool platform_open_url(const char *url) {
    wchar_t address[2048];

    return wide(url, address, 2048) &&
           (INT_PTR)ShellExecuteW(NULL, L"open", address, NULL, NULL, SW_SHOWNORMAL) > 32;
}

static void credential_name(const char *account, wchar_t name[128]) {
    wchar_t id[64] = {0};

    wide(account, id, 64);
    swprintf(name, 128, L"NativeController:Spotify:%ls", id);
}

bool platform_secret_load(const char *account, char *out, size_t capacity) {
    wchar_t name[128];
    credential_name(account, name);

    PCREDENTIALW credential = NULL;

    if (!CredReadW(name, CRED_TYPE_GENERIC, 0, &credential)) {
        return false;
    }

    bool ok = credential->CredentialBlobSize > 0 && credential->CredentialBlobSize < capacity &&
              !memchr(credential->CredentialBlob, 0, credential->CredentialBlobSize);

    if (ok) {
        memcpy(out, credential->CredentialBlob, credential->CredentialBlobSize);
        out[credential->CredentialBlobSize] = 0;
    }

    platform_wipe(credential->CredentialBlob, credential->CredentialBlobSize);
    CredFree(credential);

    return ok;
}

bool platform_secret_save(const char *account, const char *secret) {
    if (strlen(secret) > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return false;
    }

    wchar_t name[128];
    credential_name(account, name);

    CREDENTIALW credential = {0};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name;
    credential.CredentialBlobSize = (DWORD)strlen(secret);
    credential.CredentialBlob = (LPBYTE)secret;
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    return CredWriteW(&credential, 0) != 0;
}

bool platform_secret_delete(const char *account) {
    wchar_t name[128];
    credential_name(account, name);

    return CredDeleteW(name, CRED_TYPE_GENERIC, 0) || GetLastError() == ERROR_NOT_FOUND;
}

typedef struct {
    void (*function)(void *);
    void *argument;
} Task;

static DWORD WINAPI run_task(LPVOID context) {
    Task task = *(Task *)context;
    free(context);

    task.function(task.argument);

    return 0;
}

bool platform_start_thread(void (*function)(void *), void *argument) {
    Task *task = malloc(sizeof(*task));

    if (!task) {
        return false;
    }

    *task = (Task){function, argument};

    HANDLE thread = CreateThread(NULL, 0, run_task, task, 0, NULL);

    if (!thread) {
        free(task);
        return false;
    }

    CloseHandle(thread);

    return true;
}

void platform_lock(void) {
    AcquireSRWLockExclusive(&mutex);
}

void platform_unlock(void) {
    ReleaseSRWLockExclusive(&mutex);
}

void platform_token_lock(void) {
    AcquireSRWLockExclusive(&token_mutex);
}

void platform_token_unlock(void) {
    ReleaseSRWLockExclusive(&token_mutex);
}
