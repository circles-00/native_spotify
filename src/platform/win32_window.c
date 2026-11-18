#include "app.h"
#include "controller.h"
#include "platform.h"
#include "ui.h"
#include "win32_canvas.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HWND window;
static UINT dpi = 96;
static UiController controller;
static WindowsCanvas backend;
static HDC paint_buffer;
static HBITMAP paint_image;
static HGDIOBJ original_image;
static int buffer_width, buffer_height;
static LONG wake_pending;
static bool timer_active;
static bool tracking;
static wchar_t pending_surrogate;

static void sync_timer(void) {
    int delay = app_tick(!IsIconic(window));

    if (delay < 0) {
        KillTimer(window, 2);
        return;
    }

    SetTimer(window, 2, (UINT)delay, NULL);
}

static int scale(float value) {
    return (int)(value * dpi / 96 + .5f);
}

static float logical(int value) {
    return (float)value * 96 / dpi;
}

static void animate(void) {
    if (controller.scene.animating && !timer_active) {
        SetTimer(window, 1, 16, NULL);
        timer_active = true;
    }

    InvalidateRect(window, NULL, FALSE);
}

static void layout(void) {
    RECT client;
    GetClientRect(window, &client);
    ui_controller_layout(&controller, logical(client.right), logical(client.bottom));
}

static void apply_cursor(void) {
    UiCursor cursor = ui_controller_cursor(&controller);
    LPCWSTR resource = cursor == UI_CURSOR_IBEAM  ? IDC_IBEAM
                       : cursor == UI_CURSOR_HAND ? IDC_HAND
                                                  : IDC_ARROW;
    SetCursor(LoadCursorW(NULL, resource));
}

static bool mapped_key(WPARAM key, SceneKey *mapped) {
    switch (key) {
    case VK_TAB:
        *mapped = KEY_TAB;
        return true;
    case VK_RETURN:
        *mapped = KEY_ENTER;
        return true;
    case VK_BACK:
        *mapped = KEY_BACKSPACE;
        return true;
    case VK_LEFT:
        *mapped = KEY_LEFT;
        return true;
    case VK_RIGHT:
        *mapped = KEY_RIGHT;
        return true;
    case VK_UP:
        *mapped = KEY_UP;
        return true;
    case VK_DOWN:
        *mapped = KEY_DOWN;
        return true;
    case VK_ESCAPE:
        *mapped = KEY_ESCAPE;
        return true;
    case VK_SPACE:
        *mapped = KEY_SPACE;
        return true;
    default:
        return false;
    }
}

static void paste_search(HWND hwnd) {
    if (!OpenClipboard(hwnd)) {
        return;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *text = data ? GlobalLock(data) : NULL;

    if (text) {
        char utf8[SEARCH_CAPACITY * 4];
        int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);

        if (written > 1) {
            ui_controller_text(&controller, utf8, (size_t)(written - 1));
        }

        GlobalUnlock(data);
    }

    CloseClipboard();
}

static void handle_char(WPARAM code) {
    if (code >= 0xD800 && code <= 0xDBFF) {
        pending_surrogate = (wchar_t)code;
        return;
    }

    wchar_t utf16[2];
    int count = 0;

    if (code >= 0xDC00 && code <= 0xDFFF) {
        if (!pending_surrogate) {
            return;
        }

        utf16[0] = pending_surrogate;
        utf16[1] = (wchar_t)code;
        pending_surrogate = 0;
        count = 2;
    } else {
        pending_surrogate = 0;

        if (code < 0x20 || code == 0x7f) {
            return;
        }

        utf16[0] = (wchar_t)code;
        count = 1;
    }

    if (!ui_controller_is_editing(&controller)) {
        return;
    }

    char utf8[8];
    int bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16, count, utf8, sizeof(utf8), NULL, NULL);

    if (bytes > 0 && ui_controller_text(&controller, utf8, (size_t)bytes)) {
        animate();
    }
}

static void paint(HWND hwnd) {
    PAINTSTRUCT state;
    HDC screen = BeginPaint(hwnd, &state);
    RECT client;
    GetClientRect(hwnd, &client);

    if (client.right <= 0 || client.bottom <= 0) {
        EndPaint(hwnd, &state);
        return;
    }

    if (!paint_buffer) {
        paint_buffer = CreateCompatibleDC(screen);
    }

    if (!paint_buffer) {
        EndPaint(hwnd, &state);
        return;
    }

    if (buffer_width != client.right || buffer_height != client.bottom) {
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = client.right;
        info.bmiHeader.biHeight = -client.bottom;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP image = CreateDIBSection(paint_buffer, &info, DIB_RGB_COLORS, &bits, NULL, 0);

        if (!image) {
            EndPaint(hwnd, &state);
            return;
        }

        HGDIOBJ previous = SelectObject(paint_buffer, image);

        if (!paint_image) {
            original_image = previous;
        }

        if (paint_image) {
            DeleteObject(paint_image);
        }

        paint_image = image;
        buffer_width = client.right;
        buffer_height = client.bottom;
    }

    Canvas canvas;
    backend.native_text = true;
    windows_canvas_begin(&backend, paint_buffer, (float)dpi / 96, &canvas);

    if (controller.scene.animating && !ui_controller_tick(&controller, GetTickCount64()) && timer_active) {
        KillTimer(hwnd, 1);
        timer_active = false;
    }

    ui_controller_draw(&controller, logical(client.right), logical(client.bottom), &canvas);
    BitBlt(screen, 0, 0, client.right, client.bottom, paint_buffer, 0, 0, SRCCOPY);
    EndPaint(hwnd, &state);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        window = hwnd;
        dpi = GetDpiForWindow(hwnd);
        ui_controller_init(&controller, GetTickCount64());
        layout();
        return 0;

    case WM_APP:
        InterlockedExchange(&wake_pending, 0);
        layout();
        sync_timer();
        animate();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint(hwnd);
        return 0;

    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            layout();
        }

        InvalidateRect(hwnd, NULL, FALSE);
        sync_timer();
        return 0;

    case WM_KILLFOCUS:
        pending_surrogate = 0;
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            apply_cursor();
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        float x = logical(GET_X_LPARAM(lparam));
        float y = logical(GET_Y_LPARAM(lparam));
        bool cursor_changed = false;
        bool changed = ui_controller_motion(&controller, x, y, GetTickCount64(), &cursor_changed);

        if (!tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            tracking = true;
        }

        if (cursor_changed) {
            apply_cursor();
        }

        if (changed || (controller.scene.animating && GetTickCount64() >= controller.scene.last_ms + 14)) {
            animate();
        }

        return 0;
    }

    case WM_MOUSELEAVE:
        tracking = false;
        ui_controller_leave(&controller, GetTickCount64());
        animate();
        return 0;

    case WM_RBUTTONDOWN:
        ui_controller_context_press(&controller, logical(GET_X_LPARAM(lparam)),
                                    logical(GET_Y_LPARAM(lparam)));
        layout();
        animate();
        return 0;

    case WM_LBUTTONDOWN:
        ui_controller_press(&controller, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)),
                            GetTickCount64());
        SetFocus(hwnd);
        SetCapture(hwnd);
        animate();
        return 0;

    case WM_LBUTTONUP:
        ui_controller_release(&controller, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)),
                              GetTickCount64());
        ReleaseCapture();
        layout();
        animate();
        return 0;

    case WM_MOUSEWHEEL: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        ui_controller_scroll(&controller, logical(point.x), logical(point.y),
                             GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1, GetTickCount64());
        animate();
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;

        if (ctrl && wparam >= 'A' && wparam <= 'Z') {
            UiShortcutResult shortcut = ui_controller_shortcut(&controller, (char)wparam, true);

            if (shortcut == UI_SHORTCUT_PASTE_SEARCH) {
                paste_search(hwnd);
                animate();
                return 0;
            }

            if (shortcut == UI_SHORTCUT_HANDLED) {
                animate();
                return 0;
            }
        }

        SceneKey mapped;

        if (mapped_key(wparam, &mapped)) {
            ui_controller_key(&controller, mapped, (GetKeyState(VK_SHIFT) & 0x8000) != 0, GetTickCount64());
            layout();
            animate();
        }

        return 0;
    }

    case WM_CHAR:
        handle_char(wparam);
        return 0;

    case WM_TIMER:
        if (wparam == 2) {
            sync_timer();
            return 0;
        }

        if (!ui_controller_tick(&controller, GetTickCount64())) {
            KillTimer(hwnd, 1);
            timer_active = false;
        }

        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        RECT minimum = {0, 0, scale(960), scale(640)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        ((MINMAXINFO *)lparam)->ptMinTrackSize =
            (POINT){minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }

    case WM_DPICHANGED: {
        dpi = HIWORD(wparam);
        RECT *suggested = (RECT *)lparam;
        windows_canvas_destroy(&backend);
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER);
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        app_shutdown();
        window = NULL;

        if (paint_buffer) {
            if (original_image) {
                SelectObject(paint_buffer, original_image);
            }

            if (paint_image) {
                DeleteObject(paint_image);
            }

            DeleteDC(paint_buffer);
            paint_buffer = NULL;
            paint_image = NULL;
            original_image = NULL;
        }

        windows_canvas_destroy(&backend);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ui_wake(void) {
    if (window && InterlockedCompareExchange(&wake_pending, 1, 0) == 0) {
        PostMessageW(window, WM_APP, 0, 0);
    }
}

void ui_paste_engine_key(void) {
    if (!OpenClipboard(window)) {
        return;
    }

    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *text = data ? GlobalLock(data) : NULL;
    char key[2048] = {0};
    int size = text ? WideCharToMultiByte(CP_UTF8, 0, text, -1, key, sizeof(key), NULL, NULL) : 0;

    if (text) {
        GlobalUnlock(data);
    }

    CloseClipboard();
    app_engine_key(key, size > 0 ? (size_t)size - 1 : 0);
    SecureZeroMemory(key, sizeof(key));
}

int ui_run(bool smoke_test) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    windows_load_font();
    dpi = GetDpiForSystem();
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW type = {0};
    type.hInstance = instance;
    type.lpfnWndProc = window_proc;
    type.lpszClassName = L"NativeSpotify";
    type.hCursor = NULL;

    if (!RegisterClassW(&type)) {
        return 1;
    }

    window = CreateWindowExW(0, type.lpszClassName, L"Native Spotify", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, scale(1280), scale(840), NULL, NULL, instance, NULL);

    if (!window) {
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetFocus(window);

    if (smoke_test) {
        DestroyWindow(window);
        return 0;
    }

    app_action(ACTION_RESTORE);

    MSG message;

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return 0;
}
