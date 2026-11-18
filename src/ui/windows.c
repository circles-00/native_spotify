#include "app.h"
#include "platform.h"
#include "ui.h"
#include "scene.h"
#include "windows_canvas.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HWND window;
static HWND input;
static WNDPROC original_input;
static HFONT input_font;
static HBRUSH input_background;
static UINT dpi = 96;
static Scene scene;
static WindowsCanvas backend;
static WindowsCanvas text_backend;
static HDC paint_buffer;
static HBITMAP paint_image;
static HGDIOBJ original_image;
static int buffer_width, buffer_height;
static RECT input_rect;
static LONG wake_pending;
static bool timer_active;
static bool tracking;
static bool editing;

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
    if (scene.animating && !timer_active) {
        SetTimer(window, 1, 16, NULL);
        timer_active = true;
    }

    InvalidateRect(window, NULL, FALSE);
}

static void layout(void) {
    RECT client;
    GetClientRect(window, &client);
    View view = app_snapshot();
    scene_layout(&scene, &view, logical(client.right), logical(client.bottom));
    artwork_reset(view.session_revision);

    BOOL visible = !scene.login_screen;
    if (IsWindowVisible(input) != visible) {
        ShowWindow(input, visible ? SW_SHOW : SW_HIDE);
    }

    int ix = scale(scene.search.x + 50);
    int iy = scale(scene.search.y + 13);
    int iw = scale(scene.search.w - 72);
    int ih = scale(24);
    if (ix != input_rect.left || iy != input_rect.top || iw != input_rect.right || ih != input_rect.bottom) {
        input_rect = (RECT){ix, iy, iw, ih};
        MoveWindow(input, ix, iy, iw, ih, TRUE);
    }

    wchar_t expected[SEARCH_CAPACITY];
    wchar_t current[SEARCH_CAPACITY];
    MultiByteToWideChar(CP_UTF8, 0, scene.query, -1, expected, SEARCH_CAPACITY);
    GetWindowTextW(input, current, SEARCH_CAPACITY);

    if (wcscmp(expected, current)) {
        editing = true;
        SetWindowTextW(input, expected);
        editing = false;
    }
}

static void fonts(void) {
    if (input_font) {
        DeleteObject(input_font);
    }

    input_font =
        CreateFontW(-scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Fira Sans");
    SendMessageW(input, WM_SETFONT, (WPARAM)input_font, TRUE);
}

static void query_changed(void) {
    if (editing) {
        return;
    }

    wchar_t value[SEARCH_CAPACITY];
    char utf8[SEARCH_CAPACITY * 4];
    GetWindowTextW(input, value, SEARCH_CAPACITY);

    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, sizeof(utf8), NULL, NULL)) {
        return;
    }

    copy_utf8(scene.query, sizeof(scene.query), utf8);
    scene_query_changed(&scene);
    animate();

    if (strlen(utf8) >= SEARCH_CAPACITY) {
        MultiByteToWideChar(CP_UTF8, 0, scene.query, -1, value, SEARCH_CAPACITY);
        editing = true;
        SetWindowTextW(input, value);
        SendMessageW(input, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
        editing = false;
    }
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
    }

    return false;
}

static void key(WPARAM value) {
    SceneKey mapped;
    if (!mapped_key(value, &mapped)) {
        return;
    }

    View view = app_snapshot();
    scene_key(&scene, &view, mapped, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    if (scene.entrance == 0) {
        scene.last_ms = GetTickCount64();
    }

    layout();
    SetFocus(scene.focus == HIT_SEARCH && !scene.login_screen ? input : window);
    animate();
}

static LRESULT CALLBACK input_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_TAB || wparam == VK_ESCAPE)) {
        key(wparam);
        return 0;
    }

    // While the Soloist panel is open, Ctrl+V means "paste key", never into the search box.
    if (message == WM_KEYDOWN && wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && scene.setup_popup) {
        ui_paste_engine_key();
        return 0;
    }

    if (message == WM_CHAR && (wparam == '\r' || wparam == '\t')) {
        return 0;
    }

    if (message == WM_SETFOCUS) {
        scene.focus = HIT_SEARCH;
        InvalidateRect(window, NULL, FALSE);
    }

    return CallWindowProcW(original_input, hwnd, message, wparam, lparam);
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

    if (scene.animating && !scene_tick(&scene, GetTickCount64()) && timer_active) {
        KillTimer(hwnd, 1);
        timer_active = false;
    }

    View view = app_snapshot();
    scene_layout(&scene, &view, logical(client.right), logical(client.bottom));

    if (scene.first_row < view.list.first ||
        (scene.first_row + ROW_SLOTS > view.list.first + view.list.rows_count &&
         view.list.first + view.list.rows_count < view.list.count) ||
        scene.library_first < view.playlists.first ||
        (scene.library_first + 24 > view.playlists.first + view.playlists.rows_count &&
         view.playlists.first + view.playlists.rows_count < view.playlists.count)) {
        view = app_snapshot();
    }

    scene_draw(&scene, &view, &canvas);
    BitBlt(screen, 0, 0, client.right, client.bottom, paint_buffer, 0, 0, SRCCOPY);
    EndPaint(hwnd, &state);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        window = hwnd;
        dpi = GetDpiForWindow(hwnd);
        scene_init(&scene);
        scene.last_ms = GetTickCount64();
        input_background = CreateSolidBrush(RGB(31, 31, 31));
        input = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                NULL, GetModuleHandleW(NULL), NULL);

        if (!input) {
            return -1;
        }

        SendMessageW(input, EM_SETLIMITTEXT, SEARCH_CAPACITY - 1, 0);
        SendMessageW(input, EM_SETCUEBANNER, TRUE, (LPARAM)L"What do you want to play?");
        original_input = (WNDPROC)SetWindowLongPtrW(input, GWLP_WNDPROC, (LONG_PTR)input_proc);
        fonts();
        layout();
        return 0;

    case WM_COMMAND:
        if ((HWND)lparam == input && HIWORD(wparam) == EN_CHANGE) {
            query_changed();
        }
        return 0;

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wparam, RGB(235, 235, 235));
        SetBkColor((HDC)wparam, RGB(31, 31, 31));
        return (LRESULT)input_background;

    case WM_APP: {
        InterlockedExchange(&wake_pending, 0);
        View view = app_snapshot();
        bool clear_query = scene.session_revision != view.session_revision;
        layout();
        if (clear_query) {
            SetWindowTextW(input, L"");
        }
        sync_timer();
        animate();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_SIZE:
        if (input && wparam != SIZE_MINIMIZED) {
            layout();
        }
        InvalidateRect(hwnd, NULL, FALSE);
        sync_timer();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, scene.hot == HIT_SEARCH ? IDC_IBEAM
                                        : scene.hot == HIT_NONE ? IDC_ARROW
                                                                : IDC_HAND));
            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        float x = logical(GET_X_LPARAM(lparam));
        float y = logical(GET_Y_LPARAM(lparam));
        HitId target = scene_hit(&scene, x, y);
        bool cursor_changed = scene.hot != target;
        bool changed = cursor_changed || scene.dragging != HIT_NONE;

        if (scene.dragging != HIT_NONE) {
            scene_pointer_motion(&scene, x, y);
        }
        scene_hover(&scene, target, GetTickCount64());

        if (!tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            tracking = true;
        }

        if (cursor_changed) {
            SetCursor(LoadCursorW(NULL, target == HIT_SEARCH ? IDC_IBEAM
                                        : target == HIT_NONE ? IDC_ARROW
                                                             : IDC_HAND));
        }

        if (changed || (scene.animating && GetTickCount64() >= scene.last_ms + 14)) {
            animate();
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking = false;
        scene_hover(&scene, HIT_NONE, GetTickCount64());
        animate();
        return 0;

    case WM_RBUTTONDOWN: {
        View view = app_snapshot();
        scene_context_press(&scene, &view, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)));
        layout();
        animate();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        View view = app_snapshot();
        scene_press(&scene, &view, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)),
                    GetTickCount64());
        SetFocus(scene.focus == HIT_SEARCH && !scene.login_screen ? input : hwnd);
        SetCapture(hwnd);
        animate();
        return 0;
    }

    case WM_LBUTTONUP: {
        View view = app_snapshot();
        scene_pointer_motion(&scene, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)));
        scene_release(&scene, &view, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)));
        ReleaseCapture();
        layout();
        SetFocus(scene.focus == HIT_SEARCH && !scene.login_screen ? input : hwnd);
        if (scene.entrance == 0) {
            scene.last_ms = GetTickCount64();
        }
        animate();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        View view = app_snapshot();
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        uint64_t now = GetTickCount64();
        if (scene.animating) {
            scene_tick(&scene, now);
        }
        scene.last_ms = now;
        scene.hot = scene_hit(&scene, logical(point.x), logical(point.y));
        scene.over_panel = scene.panel != PANEL_NONE && logical(point.x) >= scene.side_panel.x &&
                           logical(point.y) >= scene.side_panel.y &&
                           logical(point.y) < scene.side_panel.y + scene.side_panel.h;
        scene.over_library = logical(point.x) < scene.sidebar.x + scene.sidebar.w &&
                             logical(point.y) >= scene.sidebar.y && logical(point.y) < scene.player.y;
        scene_scroll(&scene, &view, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1);
        animate();
        return 0;
    }

    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'V' && scene.setup_popup) {
            ui_paste_engine_key();
            return 0;
        }

        if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == 'L') {
            if (scene.login_screen) {
                return 0;
            }
            scene.focus = HIT_SEARCH;
            layout();
            SetFocus(input);
            SendMessageW(input, EM_SETSEL, 0, -1);
            return 0;
        }

        key(wparam);
        return 0;

    case WM_TIMER:
        if (wparam == 2) {
            sync_timer();
            return 0;
        }

        if (!scene_tick(&scene, GetTickCount64())) {
            KillTimer(hwnd, 1);
            timer_active = false;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        RECT minimum = {0, 0, scale(960), scale(640)};
        AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0, dpi);
        ((MINMAXINFO *)lparam)->ptMinTrackSize =
            (POINT){minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }

    case WM_DPICHANGED: {
        dpi = HIWORD(wparam);
        RECT *suggested = (RECT *)lparam;
        windows_canvas_destroy(&backend);
        windows_canvas_destroy(&text_backend);
        fonts();
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER);
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_DESTROY:
        app_shutdown();
        if (paint_buffer) {
            if (original_image) {
                SelectObject(paint_buffer, original_image);
            }
            if (paint_image) {
                DeleteObject(paint_image);
            }
            DeleteDC(paint_buffer);
        }

        windows_canvas_destroy(&backend);
        windows_canvas_destroy(&text_backend);
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

    window =
        CreateWindowExW(0, type.lpszClassName, L"Native Spotify", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, scale(1280), scale(840), NULL, NULL, instance, NULL);
    if (!window) {
        return 1;
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetFocus(scene.login_screen ? window : input);

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

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    (void)instance;
    (void)previous;
    (void)command;
    (void)show;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!platform_init()) {
        return 1;
    }
    app_initialize();

    return ui_run(getenv("NATIVE_CONTROLLER_SMOKE") != NULL);
}
