#include "platform.h"

#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool wide_path(const char *text, wchar_t *out, int capacity) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, capacity) > 0;
}

bool platform_make_dir(const char *path) {
    wchar_t partial[PLATFORM_PATH_CAPACITY];

    if (!wide_path(path, partial, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    // Create missing parents; skip the drive or UNC prefix.
    for (wchar_t *p = partial + 3; *p; ++p) {
        if (*p != L'\\') {
            continue;
        }

        *p = 0;
        CreateDirectoryW(partial, NULL);
        *p = L'\\';
    }

    if (!CreateDirectoryW(partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    DWORD attributes = GetFileAttributesW(partial);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool platform_data_dir(char *out, size_t capacity) {
    PWSTR base = NULL;

    // %LOCALAPPDATA% is per user and not readable by other standard accounts.
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &base))) {
        return false;
    }

    wchar_t path[PLATFORM_PATH_CAPACITY];
    int size = swprintf(path, PLATFORM_PATH_CAPACITY, L"%ls\\NativeSpotify", base);
    CoTaskMemFree(base);

    return size > 0 &&
           WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, out, (int)capacity, NULL, NULL) > 0 &&
           platform_make_dir(out);
}

bool platform_file_exists(const char *path) {
    wchar_t name[PLATFORM_PATH_CAPACITY];

    if (!wide_path(path, name, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    DWORD attributes = GetFileAttributesW(name);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool platform_read_file(const char *path, char *out, size_t capacity) {
    wchar_t name[PLATFORM_PATH_CAPACITY];

    if (!wide_path(path, name, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    HANDLE file = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD size = 0;
    BOOL ok = ReadFile(file, out, (DWORD)(capacity - 1), &size, NULL);
    CloseHandle(file);

    if (!ok) {
        return false;
    }

    out[size] = 0;
    return true;
}

bool platform_write_file(const char *path, const char *text) {
    wchar_t name[PLATFORM_PATH_CAPACITY];

    if (!wide_path(path, name, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, text, (DWORD)strlen(text), &written, NULL);

    return CloseHandle(file) && ok && written == strlen(text);
}

bool platform_download(const char *url, const char *path, size_t limit) {
    wchar_t address[2048];
    wchar_t host[256];
    wchar_t resource[2048];
    wchar_t name[PLATFORM_PATH_CAPACITY];

    if (!wide_path(url, address, 2048) || !wide_path(path, name, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    URL_COMPONENTS parts = {0};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = resource;
    parts.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(address, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"NativeController/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

    if (!session) {
        return false;
    }

    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection
                            ? WinHttpOpenRequest(connection, L"GET", resource, NULL, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD status = 0;
    DWORD size = sizeof(status);
    size_t written = 0;
    bool ok =
        request && WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, NULL) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200;

    if (ok) {
        file = CreateFileW(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = file != INVALID_HANDLE_VALUE;
    }

    char buffer[65536];

    while (ok) {
        DWORD received = 0;

        if (!WinHttpReadData(request, buffer, sizeof(buffer), &received)) {
            ok = false;
            break;
        }

        if (!received) {
            break;
        }

        DWORD stored = 0;
        ok = received <= limit - written && WriteFile(file, buffer, received, &stored, NULL) &&
             stored == received;
        written += received;
    }

    if (file != INVALID_HANDLE_VALUE) {
        ok = CloseHandle(file) && ok;
    }

    ok = ok && written > 0;

    if (!ok) {
        DeleteFileW(name);
    }

    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }

    WinHttpCloseHandle(session);
    return ok;
}

/* Quotes one argument following the CommandLineToArgvW / CRT parsing rules. */
static bool quote_argument(wchar_t *out, size_t capacity, size_t *length, const wchar_t *argument) {
    size_t used = *length;

    if (used + 1 >= capacity) {
        return false;
    }

    if (used) {
        out[used++] = L' ';
    }

    out[used++] = L'"';

    for (const wchar_t *p = argument;; ++p) {
        size_t backslashes = 0;

        while (*p == L'\\') {
            ++backslashes;
            ++p;
        }

        // Backslashes are literal unless they precede a quote or the closing quote.
        size_t repeat = !*p || *p == L'"' ? backslashes * 2 + (*p == L'"') : backslashes;

        if (used + repeat + 3 >= capacity) {
            return false;
        }

        for (size_t i = 0; i < repeat; ++i) {
            out[used++] = L'\\';
        }

        if (!*p) {
            break;
        }

        out[used++] = *p;
    }

    out[used++] = L'"';
    out[used] = 0;
    *length = used;
    return true;
}

static HANDLE spawn(const wchar_t *path, const wchar_t *command, HANDLE job) {
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE quiet = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inherit, OPEN_EXISTING, 0, NULL);

    if (quiet == INVALID_HANDLE_VALUE) {
        return NULL;
    }

    STARTUPINFOEXW startup = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = quiet;
    startup.StartupInfo.hStdOutput = quiet;
    startup.StartupInfo.hStdError = quiet;
    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributes_size);
    startup.lpAttributeList = malloc(attributes_size);
    bool initialized = startup.lpAttributeList &&
                       InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size);
    PROCESS_INFORMATION child = {0};
    wchar_t *mutable_command = _wcsdup(command);

    // Only the NUL handle is inherited; nothing else from this process leaks to the child.
    bool ok = initialized && mutable_command &&
              UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &quiet,
                                        sizeof(quiet), NULL, NULL) &&
              CreateProcessW(path, mutable_command, NULL, NULL, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL,
                             &startup.StartupInfo, &child);

    if (initialized) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    }

    free(startup.lpAttributeList);
    free(mutable_command);
    CloseHandle(quiet);

    if (!ok) {
        return NULL;
    }

    if ((job && !AssignProcessToJobObject(job, child.hProcess)) || ResumeThread(child.hThread) == (DWORD)-1) {
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return NULL;
    }

    CloseHandle(child.hThread);
    return child.hProcess;
}

bool platform_extract(const char *archive, const char *directory) {
    wchar_t tool[MAX_PATH];
    wchar_t source[PLATFORM_PATH_CAPACITY];
    wchar_t target[PLATFORM_PATH_CAPACITY];
    UINT length = GetSystemDirectoryW(tool, MAX_PATH);

    if (!length || length + 9 >= MAX_PATH || !wide_path(archive, source, PLATFORM_PATH_CAPACITY) ||
        !wide_path(directory, target, PLATFORM_PATH_CAPACITY)) {
        return false;
    }

    // Windows 10+ ships bsdtar, which also reads ZIP archives.
    wcscat(tool, L"\\tar.exe");
    wchar_t command[4 * PLATFORM_PATH_CAPACITY];
    size_t used = 0;
    const wchar_t *arguments[] = {tool, L"-xf", source, L"-C", target};

    for (unsigned i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        if (!quote_argument(command, sizeof(command) / sizeof(command[0]), &used, arguments[i])) {
            return false;
        }
    }

    HANDLE child = spawn(tool, command, NULL);

    if (!child) {
        return false;
    }

    DWORD code = 1;
    bool ok =
        WaitForSingleObject(child, 120000) == WAIT_OBJECT_0 && GetExitCodeProcess(child, &code) && code == 0;

    if (!ok) {
        TerminateProcess(child, 1);
    }

    CloseHandle(child);
    return ok;
}

struct PlatformProcess {
    HANDLE child;
    HANDLE job;
    HANDLE watcher;
    volatile LONG stopping;
    ProcessExit exited_callback;
    void *context;
};

static DWORD WINAPI watch_process(void *argument) {
    PlatformProcess *process = argument;
    DWORD code = 1;

    WaitForSingleObject(process->child, INFINITE);
    GetExitCodeProcess(process->child, &code);

    if (!InterlockedCompareExchange(&process->stopping, 0, 0)) {
        process->exited_callback((int)code, process->context);
    }

    return 0;
}

PlatformProcess *platform_process_start(const char *path, const char *const *arguments, ProcessExit exited,
                                        void *context) {
    wchar_t executable[PLATFORM_PATH_CAPACITY];
    wchar_t command[8192];
    wchar_t argument[2048];
    size_t used = 0;

    if (!wide_path(path, executable, PLATFORM_PATH_CAPACITY)) {
        return NULL;
    }

    for (unsigned i = 0; arguments[i]; ++i) {
        if (!wide_path(arguments[i], argument, 2048) ||
            !quote_argument(command, sizeof(command) / sizeof(command[0]), &used, argument)) {
            SecureZeroMemory(command, sizeof(command));
            SecureZeroMemory(argument, sizeof(argument));
            return NULL;
        }
    }

    SecureZeroMemory(argument, sizeof(argument));
    PlatformProcess *process = calloc(1, sizeof(*process));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (process) {
        process->exited_callback = exited;
        process->context = context;
        process->job = CreateJobObjectW(NULL, NULL);
    }

    // The job closes with this process, so the player never outlives the app.
    bool ok =
        process && process->job &&
        SetInformationJobObject(process->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    if (ok) {
        process->child = spawn(executable, command, process->job);
        ok = process->child != NULL;
    }

    SecureZeroMemory(command, sizeof(command));

    if (ok) {
        process->watcher = CreateThread(NULL, 0, watch_process, process, 0, NULL);
        ok = process->watcher != NULL;
    }

    if (!ok) {
        if (process && process->child) {
            TerminateJobObject(process->job, 1);
            CloseHandle(process->child);
        }
        if (process && process->job) {
            CloseHandle(process->job);
        }

        free(process);
        return NULL;
    }

    return process;
}

void platform_process_stop(PlatformProcess *process) {
    if (!process) {
        return;
    }

    InterlockedExchange(&process->stopping, 1);
    TerminateJobObject(process->job, 0);
    WaitForSingleObject(process->watcher, INFINITE);
    CloseHandle(process->watcher);
    CloseHandle(process->child);
    CloseHandle(process->job);
    free(process);
}
