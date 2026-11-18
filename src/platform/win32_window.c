#define COBJMACROS

#include "app.h"
#include "canvas.h"
#include "controller.h"
#include "platform.h"
#include "ui.h"
#include "win32_canvas.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HWND window;
static UINT dpi = 96;
static float scene_scale = 1.0f;
static UiController controller;
static WindowsCanvas backend;
static WindowsBuffer paint_buffer;

typedef enum { CAPTION_NONE, CAPTION_MINIMIZE, CAPTION_MAXIMIZE, CAPTION_CLOSE } CaptionButton;

enum { CAPTION_BUTTON_WIDTH = 40, CAPTION_BUTTON_HEIGHT = 38 };
static CaptionButton caption_hover, caption_pressed;
static bool painted;
static LONG wake_pending;
static bool timer_active;
static bool tracking;
static wchar_t pending_surrogate;
static bool lyrics_fullscreen;
static LONG_PTR windowed_style;
static WINDOWPLACEMENT windowed_placement;

enum { THUMB_PREVIOUS = 1, THUMB_PLAYBACK, THUMB_NEXT, THUMB_PAUSE_ICON };

static const struct {
    Icon icon;
    const wchar_t *tip;
    const char *command;
    Action action;
} thumb_controls[] = {
    {ICON_PREVIOUS, L"Previous track", "previous", ACTION_PREVIOUS},
    {ICON_PLAY, L"Play", "play", ACTION_TOGGLE_PLAYBACK},
    {ICON_NEXT, L"Next track", "next", ACTION_NEXT},
    {ICON_PAUSE, L"Pause", "pause", ACTION_TOGGLE_PLAYBACK},
};

static UINT taskbar_created;
static bool taskbar_com_initialized;
static ITaskbarList3 *taskbar;
static HICON thumb_icons[THUMB_PAUSE_ICON];
static bool thumb_added;
static unsigned thumb_state;

static HICON make_thumb_icon(Icon icon) {
    int width = GetSystemMetrics(SM_CXSMICON);
    int height = GetSystemMetrics(SM_CYSMICON);
    WindowsBuffer buffer = {0};
    HICON result = NULL;

    if (windows_buffer_resize(&buffer, NULL, width, height)) {
        size_t count = (size_t)width * height;
        uint32_t *pixels = buffer.pixels;
        memset(pixels, 0, count * sizeof(*pixels));
        WindowsCanvas renderer = {0};
        Canvas canvas;
        windows_canvas_begin(&renderer, &buffer, 1.0f, &canvas);
        canvas_icon(&canvas, icon, (Rect){0, 0, (float)width, (float)height}, 0xffffffffu);

        // The canvas blends onto RGB; convert white-on-black coverage into premultiplied icon pixels.
        for (size_t i = 0; i < count; ++i) {
            uint32_t alpha = pixels[i] & 0xff;
            pixels[i] = (alpha << 24) | (alpha * 0x010101u);
        }

        size_t mask_stride = ((size_t)width + 15) / 16 * 2;
        BYTE *bits = calloc((size_t)height, mask_stride);

        if (bits) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    if (!pixels[(size_t)y * width + x]) {
                        bits[(size_t)y * mask_stride + x / 8] |= (BYTE)(0x80 >> (x % 8));
                    }
                }
            }

            HBITMAP mask = CreateBitmap(width, height, 1, 1, bits);
            free(bits);

            if (mask) {
                ICONINFO info = {.fIcon = TRUE, .hbmMask = mask, .hbmColor = buffer.bitmap};
                SelectObject(buffer.dc, buffer.previous);
                result = CreateIconIndirect(&info);
                DeleteObject(mask);
            }
        }

        windows_canvas_destroy(&renderer);
    }

    windows_buffer_destroy(&buffer);
    return result;
}

static void update_thumb_buttons(void) {
    if (!taskbar) {
        return;
    }

    const View *view = controller.snapshot;
    bool playing = view && view->authenticated && view->playback.playing;
    unsigned state = playing ? 1u << THUMB_PAUSE_ICON : 0;
    THUMBBUTTON buttons[THUMB_NEXT] = {0};

    for (unsigned id = THUMB_PREVIOUS; id <= THUMB_NEXT; ++id) {
        unsigned index = (id == THUMB_PLAYBACK && playing ? THUMB_PAUSE_ICON : id) - 1;
        bool enabled = view && view->authenticated &&
                       playback_can_command(&view->playback, thumb_controls[index].command);
        THUMBBUTTON *button = &buttons[id - 1];
        button->dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
        button->iId = id;
        button->hIcon = thumb_icons[index];
        button->dwFlags = enabled ? THBF_ENABLED : THBF_DISABLED;
        lstrcpynW(button->szTip, thumb_controls[index].tip, ARRAYSIZE(button->szTip));

        if (enabled) {
            state |= 1u << id;
        }
    }

    if (!thumb_added) {
        thumb_added = SUCCEEDED(ITaskbarList3_ThumbBarAddButtons(taskbar, window, THUMB_NEXT, buttons));

        if (thumb_added) {
            thumb_state = state;
        }
    } else if (state != thumb_state &&
               SUCCEEDED(ITaskbarList3_ThumbBarUpdateButtons(taskbar, window, THUMB_NEXT, buttons))) {
        thumb_state = state;
    }
}

static void sync_timer(void) {
    // The taskbar preview is still interactive while the window is minimized.
    int delay = app_tick(!IsIconic(window) || thumb_added);

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
    return (float)value / scene_scale;
}

static Rect caption_rect(CaptionButton button) {
    Rect header = controller.scene.header;
    return (Rect){header.w - (CAPTION_CLOSE - button + 1) * CAPTION_BUTTON_WIDTH,
                  (header.h - CAPTION_BUTTON_HEIGHT) * 0.5f, CAPTION_BUTTON_WIDTH, CAPTION_BUTTON_HEIGHT};
}

static CaptionButton caption_hit(float x, float y) {
    if (!lyrics_fullscreen) {
        for (CaptionButton button = CAPTION_MINIMIZE; button <= CAPTION_CLOSE; ++button) {
            if (scene_contains(caption_rect(button), x, y)) {
                return button;
            }
        }
    }

    return CAPTION_NONE;
}

static int top_frame(UINT window_dpi) {
    return GetSystemMetricsForDpi(SM_CYSIZEFRAME, window_dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, window_dpi);
}

static LRESULT frame_hit(HWND hwnd, LPARAM position) {
    if (lyrics_fullscreen) {
        return HTCLIENT;
    }

    POINT point = {GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(hwnd, &point);
    RECT client;
    GetClientRect(hwnd, &client);

    if (!IsZoomed(hwnd) && point.y < top_frame(dpi) && point.y >= 0) {
        int corner = top_frame(dpi);

        if (point.x < corner) {
            return HTTOPLEFT;
        }

        if (point.x >= client.right - corner) {
            return HTTOPRIGHT;
        }

        return HTTOP;
    }

    if (!PtInRect(&client, point)) {
        return DefWindowProcW(hwnd, WM_NCHITTEST, 0, position);
    }

    const Scene *scene = &controller.scene;
    float x = logical(point.x), y = logical(point.y);

    if (!scene_contains(scene->header, x, y) || caption_hit(x, y) || scene->menu.open || scene->setup_popup) {
        return HTCLIENT;
    }

    const HitId controls[] = {HIT_HOME, HIT_BACK, HIT_FORWARD, HIT_TOP_HOME, HIT_SEARCH, HIT_PLAYING_ON,
                              HIT_PROFILE};

    for (unsigned i = 0; i < ARRAYSIZE(controls); ++i) {
        if (scene_contains(scene->hits[controls[i]].rect, x, y)) {
            return HTCLIENT;
        }
    }

    return HTCAPTION;
}

static void draw_caption(HWND hwnd, Canvas *canvas) {
    if (lyrics_fullscreen) {
        return;
    }

    void *context = canvas->context;

    for (CaptionButton button = CAPTION_MINIMIZE; button <= CAPTION_CLOSE; ++button) {
        Rect rect = caption_rect(button);

        if (caption_hover == button && (!caption_pressed || caption_pressed == button)) {
            Color fill = button == CAPTION_CLOSE ? (caption_pressed ? 0xb61723 : 0xc42b36)
                                                  : (caption_pressed ? 0x414951 : 0x31373e);
            canvas->box(context, rect, 6, fill);
        }

        float x = rect.x + rect.w * 0.5f, y = rect.y + rect.h * 0.5f;
        Color ink = 0xe7ebef;

        if (button == CAPTION_MINIMIZE) {
            canvas->box(context, (Rect){x - 5, y, 10, 1}, 0, ink);
        } else if (button == CAPTION_MAXIMIZE) {
            if (IsZoomed(hwnd)) {
                Point back[] = {{x - 2, y - 4}, {x - 2, y - 6}, {x + 6, y - 6}, {x + 6, y + 2}, {x + 4, y + 2}};
                Point front[] = {{x - 5, y - 3}, {x + 3, y - 3}, {x + 3, y + 5}, {x - 5, y + 5}};
                canvas->path(context, back, ARRAYSIZE(back), false, false, 1, ink);
                canvas->path(context, front, ARRAYSIZE(front), true, false, 1, ink);
            } else {
                Point square[] = {{x - 5, y - 5}, {x + 5, y - 5}, {x + 5, y + 5}, {x - 5, y + 5}};
                canvas->path(context, square, ARRAYSIZE(square), true, false, 1, ink);
            }
        } else {
            Point down[] = {{x - 5, y - 5}, {x + 5, y + 5}};
            Point up[] = {{x - 5, y + 5}, {x + 5, y - 5}};
            canvas->path(context, down, ARRAYSIZE(down), false, false, 1, ink);
            canvas->path(context, up, ARRAYSIZE(up), false, false, 1, ink);
        }
    }
}

static void animate(void) {
    if (lyrics_fullscreen != controller.scene.lyrics_fullscreen) {
        lyrics_fullscreen = controller.scene.lyrics_fullscreen;
        caption_hover = CAPTION_NONE;

        if (lyrics_fullscreen) {
            MONITORINFO monitor = {.cbSize = sizeof(monitor)};
            windowed_placement.length = sizeof(windowed_placement);

            if (GetWindowPlacement(window, &windowed_placement) &&
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                windowed_style = GetWindowLongPtrW(window, GWL_STYLE);
                SetWindowLongPtrW(window, GWL_STYLE, windowed_style & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
                RECT bounds = monitor.rcMonitor;
                SetWindowPos(window, HWND_TOP, bounds.left, bounds.top, bounds.right - bounds.left,
                             bounds.bottom - bounds.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            } else {
                windowed_style = 0;
            }
        } else if (windowed_style) {
            SetWindowLongPtrW(window, GWL_STYLE, windowed_style);
            SetWindowPlacement(window, &windowed_placement);
            SetWindowPos(window, NULL, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
        }
    }

    if (controller.scene.animating && !timer_active) {
        SetTimer(window, 1, 16, NULL);
        timer_active = true;
    }

    InvalidateRect(window, NULL, FALSE);
}

static void layout(void) {
    RECT client;
    GetClientRect(window, &client);
    scene_scale = scene_view_scale((float)client.right, (float)client.bottom, (float)dpi / 96);
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
    case VK_DELETE:
        *mapped = KEY_DELETE;
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
    case VK_HOME:
        *mapped = KEY_HOME;
        return true;
    case VK_END:
        *mapped = KEY_END;
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

static void copy_search(HWND hwnd, const char *utf8) {
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);

    if (wide_len <= 0 || !OpenClipboard(hwnd)) {
        return;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (size_t)wide_len * sizeof(wchar_t));
    wchar_t *target = memory ? GlobalLock(memory) : NULL;

    if (target) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, target, wide_len);
        GlobalUnlock(memory);
        EmptyClipboard();

        if (!SetClipboardData(CF_UNICODETEXT, memory)) {
            GlobalFree(memory);
        }
    } else if (memory) {
        GlobalFree(memory);
    }

    CloseClipboard();
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
            size_t len = (size_t)(written - 1);

            while (len > 0 && (utf8[len - 1] == '\n' || utf8[len - 1] == '\r')) {
                --len;
            }

            if (len > 0) {
                ui_controller_text(&controller, utf8, len);
            }
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

    if (!windows_buffer_resize(&paint_buffer, screen, client.right, client.bottom)) {
        EndPaint(hwnd, &state);
        return;
    }

    Canvas canvas;
    backend.native_text = true;
    scene_scale = scene_view_scale((float)client.right, (float)client.bottom, (float)dpi / 96);
    windows_canvas_begin(&backend, &paint_buffer, scene_scale, &canvas);

    if (controller.scene.animating && !ui_controller_tick(&controller, GetTickCount64()) && timer_active) {
        KillTimer(hwnd, 1);
        timer_active = false;
    }

    ui_controller_draw(&controller, logical(client.right), logical(client.bottom), &canvas);
    draw_caption(hwnd, &canvas);
    painted = BitBlt(screen, 0, 0, client.right, client.bottom, paint_buffer.dc, 0, 0, SRCCOPY) != 0;
    EndPaint(hwnd, &state);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // Explorer sends this again when it recreates the taskbar.
    if (taskbar_created && message == taskbar_created) {
        thumb_added = false;

        if (taskbar) {
            ITaskbarList3_Release(taskbar);
            taskbar = NULL;
        }

        if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
                                       (void **)&taskbar))) {
            if (SUCCEEDED(ITaskbarList3_HrInit(taskbar))) {
                update_thumb_buttons();
            } else {
                ITaskbarList3_Release(taskbar);
                taskbar = NULL;
            }
        }

        sync_timer();
        return 0;
    }

    switch (message) {
    case WM_NCCALCSIZE: {
        RECT *client = wparam ? &((NCCALCSIZE_PARAMS *)lparam)->rgrc[0] : (RECT *)lparam;
        LONG top = client->top;
        DefWindowProcW(hwnd, message, wparam, lparam);

        if (!lyrics_fullscreen && !IsIconic(hwnd)) {
            // Keep the native side/bottom resize frame, but let the app own the caption.
            client->top = top + (IsZoomed(hwnd) ? top_frame(GetDpiForWindow(hwnd)) : 0);
        }

        return 0;
    }

    case WM_NCHITTEST:
        return frame_hit(hwnd, lparam);

    case WM_CREATE:
        window = hwnd;
        dpi = GetDpiForWindow(hwnd);
        taskbar_com_initialized = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));

        if (taskbar_com_initialized) {
            taskbar_created = RegisterWindowMessageW(L"TaskbarButtonCreated");

            for (unsigned i = 0; i < THUMB_PAUSE_ICON; ++i) {
                thumb_icons[i] = make_thumb_icon(thumb_controls[i].icon);

                if (!thumb_icons[i]) {
                    taskbar_created = 0;
                    break;
                }
            }
        }

        ui_controller_init(&controller, GetTickCount64());
        controller.scene.caption_width = CAPTION_CLOSE * CAPTION_BUTTON_WIDTH;
        layout();
        return 0;

    case WM_APP:
        InterlockedExchange(&wake_pending, 0);
        layout();
        update_thumb_buttons();
        sync_timer();
        animate();
        return 0;

    case WM_COMMAND:
        if (HIWORD(wparam) == THBN_CLICKED && !lparam) {
            unsigned id = LOWORD(wparam);

            if (thumb_added && id >= THUMB_PREVIOUS && id <= THUMB_NEXT && (thumb_state & (1u << id))) {
                app_action(thumb_controls[id - 1].action);
            }

            return 0;
        }
        break;

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
            if (caption_hover || caption_pressed) {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            } else {
                apply_cursor();
            }

            return TRUE;
        }
        break;

    case WM_MOUSEMOVE: {
        float x = logical(GET_X_LPARAM(lparam));
        float y = logical(GET_Y_LPARAM(lparam));
        CaptionButton hover = caption_hit(x, y);

        if (!tracking) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            tracking = true;
        }

        if (hover != caption_hover) {
            caption_hover = hover;
            InvalidateRect(hwnd, NULL, FALSE);
        }

        if (caption_pressed || (hover && GetCapture() != hwnd)) {
            ui_controller_leave(&controller, GetTickCount64());
            SetCursor(LoadCursorW(NULL, IDC_ARROW));

            if (controller.scene.animating) {
                animate();
            }

            return 0;
        }

        bool cursor_changed = false;
        bool changed = ui_controller_motion(&controller, x, y, GetTickCount64(), &cursor_changed);

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
        caption_hover = CAPTION_NONE;
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
        caption_pressed = caption_hit(logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)));

        if (caption_pressed) {
            caption_hover = caption_pressed;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        ui_controller_press(&controller, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)),
                            GetTickCount64());
        SetFocus(hwnd);
        SetCapture(hwnd);
        animate();
        return 0;

    case WM_LBUTTONUP:
        if (caption_pressed) {
            CaptionButton pressed = caption_pressed;
            caption_pressed = CAPTION_NONE;
            caption_hover = caption_hit(logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)));
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);

            if (pressed == caption_hover) {
                const WPARAM commands[] = {0, SC_MINIMIZE, IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE, SC_CLOSE};
                PostMessageW(hwnd, WM_SYSCOMMAND, commands[pressed], 0);
            }

            return 0;
        }

        ui_controller_release(&controller, logical(GET_X_LPARAM(lparam)), logical(GET_Y_LPARAM(lparam)),
                              GetTickCount64());
        ReleaseCapture();
        layout();
        animate();
        return 0;

    case WM_CAPTURECHANGED:
        if (caption_pressed) {
            caption_pressed = CAPTION_NONE;
            caption_hover = CAPTION_NONE;
            InvalidateRect(hwnd, NULL, FALSE);
        }

        break;

    case WM_MOUSEWHEEL: {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &point);
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        bool page = lines == WHEEL_PAGESCROLL;
        ui_controller_wheel(&controller, logical(point.x), logical(point.y),
                            -(float)GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA, page ? 0 : lines, page,
                            GetTickCount64());
        animate();
        return 0;
    }

    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (ctrl && wparam >= 'A' && wparam <= 'Z') {
            UiShortcutResult shortcut = ui_controller_shortcut(&controller, (char)wparam, true);

            if (shortcut == UI_SHORTCUT_PASTE_SEARCH) {
                paste_search(hwnd);
                layout();
                animate();
                return 0;
            }

            if (shortcut == UI_SHORTCUT_COPY_SEARCH || shortcut == UI_SHORTCUT_CUT_SEARCH) {
                char selected[SEARCH_CAPACITY];

                if (ui_controller_copy(&controller, selected, sizeof(selected),
                                       shortcut == UI_SHORTCUT_CUT_SEARCH)) {
                    copy_search(hwnd, selected);
                    layout();
                    animate();
                }

                return 0;
            }

            if (shortcut == UI_SHORTCUT_HANDLED) {
                layout();
                animate();
                return 0;
            }
        }

        SceneKey mapped;

        if (mapped_key(wparam, &mapped)) {
            ui_controller_key_mods(&controller, mapped, shift, ctrl, GetTickCount64());
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
            (POINT){minimum.right - minimum.left, minimum.bottom};
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
        if (taskbar) {
            ITaskbarList3_Release(taskbar);
            taskbar = NULL;
        }

        for (unsigned i = 0; i < THUMB_PAUSE_ICON; ++i) {
            if (thumb_icons[i]) {
                DestroyIcon(thumb_icons[i]);
            }
        }

        if (taskbar_com_initialized) {
            CoUninitialize();
        }

        app_shutdown();
        ui_controller_destroy(&controller);
        window = NULL;

        windows_canvas_destroy(&backend);
        windows_buffer_destroy(&paint_buffer);
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
    HICON big_icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                       GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON small_icon =
        (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                          GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    WNDCLASSW type = {0};
    type.hInstance = instance;
    type.lpfnWndProc = window_proc;
    type.lpszClassName = L"NativeSpotify";
    type.hCursor = NULL;
    type.hIcon = big_icon;

    if (!RegisterClassW(&type)) {
        return 1;
    }

    window = CreateWindowExW(0, type.lpszClassName, L"Native Spotify", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, scale(1280), scale(840), NULL, NULL, instance, NULL);

    if (!window) {
        return 1;
    }

    if (big_icon) {
        SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)big_icon);
    }

    if (small_icon) {
        SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    }

    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetFocus(window);

    if (smoke_test) {
        DestroyWindow(window);
        return painted ? 0 : 1;
    }

    app_action(ACTION_RESTORE);

    MSG message;

    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return 0;
}
