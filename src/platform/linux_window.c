#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "controller.h"
#include "linux_canvas.h"
#include "ui.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <cairo/cairo-xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int notifications[2] = {-1, -1};

typedef struct {
    Display *display;
    Window handle;
    cairo_surface_t *surface;
    cairo_surface_t *buffer;
    XIC input;
    Atom clipboard, utf8, paste, key_paste, close;
    Cursor hand, caret;
    bool focused, input_active, visible;
    int width, height;
    UiController controller;
    LinuxCanvas backend;
} NativeWindow;

static NativeWindow *active_window;

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void input_focus(NativeWindow *window) {
    bool active = window->focused && ui_controller_is_editing(&window->controller);

    if (!window->input || active == window->input_active) {
        return;
    }

    if (active) {
        XSetICFocus(window->input);
    }

    if (!active) {
        XUnsetICFocus(window->input);
    }

    window->input_active = active;
}

static void apply_cursor(NativeWindow *window) {
    UiCursor cursor = ui_controller_cursor(&window->controller);

    if (cursor == UI_CURSOR_IBEAM) {
        XDefineCursor(window->display, window->handle, window->caret);
        return;
    }

    if (cursor == UI_CURSOR_HAND) {
        XDefineCursor(window->display, window->handle, window->hand);
        return;
    }

    XUndefineCursor(window->display, window->handle);
}

static void draw(NativeWindow *window) {
    if (!window->buffer || cairo_image_surface_get_width(window->buffer) != window->width ||
        cairo_image_surface_get_height(window->buffer) != window->height) {
        if (window->buffer) {
            cairo_surface_destroy(window->buffer);
        }

        window->buffer = cairo_image_surface_create(CAIRO_FORMAT_RGB24, window->width, window->height);
    }

    cairo_t *paint = cairo_create(window->buffer);
    Canvas canvas;
    linux_canvas_begin(&window->backend, paint, &canvas);
    ui_controller_draw(&window->controller, (float)window->width, (float)window->height, &canvas);
    linux_canvas_end(&window->backend);
    input_focus(window);

    cairo_t *output = cairo_create(window->surface);
    cairo_set_source_surface(output, window->buffer, 0, 0);
    cairo_paint(output);
    cairo_destroy(output);
    cairo_destroy(paint);
    cairo_surface_flush(window->surface);
    XFlush(window->display);
}

void ui_wake(void) {
    if (notifications[1] >= 0) {
        ssize_t count = write(notifications[1], "!", 1);
        (void)count;
    }
}

static bool mapped_key(KeySym symbol, SceneKey *mapped) {
    switch (symbol) {
    case XK_Tab:
    case XK_ISO_Left_Tab:
        *mapped = KEY_TAB;
        return true;
    case XK_Return:
    case XK_KP_Enter:
        *mapped = KEY_ENTER;
        return true;
    case XK_BackSpace:
        *mapped = KEY_BACKSPACE;
        return true;
    case XK_Left:
        *mapped = KEY_LEFT;
        return true;
    case XK_Right:
        *mapped = KEY_RIGHT;
        return true;
    case XK_Up:
        *mapped = KEY_UP;
        return true;
    case XK_Down:
        *mapped = KEY_DOWN;
        return true;
    case XK_Escape:
        *mapped = KEY_ESCAPE;
        return true;
    case XK_space:
        *mapped = KEY_SPACE;
        return true;
    default:
        return false;
    }
}

static void key(NativeWindow *window, XKeyEvent *event) {
    char text[SEARCH_CAPACITY];
    KeySym symbol = NoSymbol;
    Status status = 0;
    int size = window->input && window->input_active
                   ? Xutf8LookupString(window->input, event, text, sizeof(text), &symbol, &status)
                   : XLookupString(event, text, sizeof(text), &symbol, NULL);

    if (status == XBufferOverflow) {
        return;
    }

    bool ctrl = (event->state & ControlMask) != 0;

    if (ctrl && ((symbol >= XK_a && symbol <= XK_z) || (symbol >= XK_A && symbol <= XK_Z))) {
        UiShortcutResult shortcut = ui_controller_shortcut(&window->controller, (char)symbol, true);

        if (shortcut == UI_SHORTCUT_PASTE_SEARCH) {
            XConvertSelection(window->display, window->clipboard, window->utf8, window->paste, window->handle,
                              event->time);
            return;
        }

        if (shortcut == UI_SHORTCUT_HANDLED) {
            return;
        }
    }

    if (ctrl && ui_controller_is_editing(&window->controller)) {
        return;
    }

    if (!ctrl && size > 0 && symbol != XK_Return && symbol != XK_Tab && symbol != XK_BackSpace &&
        symbol != XK_Escape && ui_controller_is_editing(&window->controller)) {
        ui_controller_text(&window->controller, text, (size_t)size);
        return;
    }

    SceneKey mapped;

    if (mapped_key(symbol, &mapped)) {
        ui_controller_key(&window->controller, mapped, (event->state & ShiftMask) != 0, milliseconds());
    }
}

void ui_paste_engine_key(void) {
    if (!active_window) {
        return;
    }

    // Requested into its own property so a key is never mistaken for search text.
    XConvertSelection(active_window->display, active_window->clipboard, active_window->utf8,
                      active_window->key_paste, active_window->handle, CurrentTime);
    XFlush(active_window->display);
}

static void paste_key(NativeWindow *window) {
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(window->display, window->handle, window->key_paste, 0, 1024, True, window->utf8,
                           &type, &format, &count, &remaining, &data) == Success &&
        type == window->utf8 && format == 8 && !remaining && data) {
        app_engine_key((const char *)data, count);
    }

    if (data) {
        memset(data, 0, count);
        XFree(data);
    }

    XDeleteProperty(window->display, window->handle, window->key_paste);
}

static void paste(NativeWindow *window, XSelectionEvent *event) {
    if (event->property == window->key_paste) {
        paste_key(window);
        return;
    }

    if (event->property != window->paste || !ui_controller_is_editing(&window->controller)) {
        return;
    }

    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(window->display, window->handle, window->paste, 0, SEARCH_CAPACITY, True,
                           window->utf8, &type, &format, &count, &remaining, &data) == Success) {
        if (type == window->utf8 && format == 8 && !remaining && data) {
            ui_controller_text(&window->controller, (const char *)data, count);
        }

        if (data) {
            XFree(data);
        }
    }

    XDeleteProperty(window->display, window->handle, window->paste);
}

int ui_run(bool smoke_test) {
    setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    linux_load_font();

    NativeWindow window = {.width = 1280, .height = 800, .visible = true};
    ui_controller_init(&window.controller, milliseconds());
    window.display = XOpenDisplay(NULL);

    if (!window.display) {
        fprintf(stderr, "No X11/XWayland display.\n");
        return 1;
    }

    if (pipe(notifications)) {
        XCloseDisplay(window.display);
        return 1;
    }

    fcntl(notifications[0], F_SETFL, O_NONBLOCK);
    fcntl(notifications[1], F_SETFL, O_NONBLOCK);

    int screen = DefaultScreen(window.display);
    window.handle = XCreateSimpleWindow(window.display, RootWindow(window.display, screen), 0, 0,
                                        (unsigned)window.width, (unsigned)window.height, 0, 0, 0);

    window.close = XInternAtom(window.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(window.display, window.handle, &window.close, 1);
    XStoreName(window.display, window.handle, "Native Spotify");
    XSizeHints hints = {.flags = PMinSize, .min_width = 960, .min_height = 640};
    XSetWMNormalHints(window.display, window.handle, &hints);

    XIM method = XOpenIM(window.display, NULL, NULL, NULL);

    if (method) {
        window.input = XCreateIC(method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                                 window.handle, XNFocusWindow, window.handle, NULL);
    }

    long filter = 0;

    if (window.input) {
        XGetICValues(window.input, XNFilterEvents, &filter, NULL);
    }

    XSelectInput(window.display, window.handle,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | FocusChangeMask | LeaveWindowMask | filter);

    window.clipboard = XInternAtom(window.display, "CLIPBOARD", False);
    window.utf8 = XInternAtom(window.display, "UTF8_STRING", False);
    window.paste = XInternAtom(window.display, "NATIVE_PASTE", False);
    window.key_paste = XInternAtom(window.display, "NATIVE_KEY_PASTE", False);
    active_window = &window;
    window.hand = XCreateFontCursor(window.display, XC_hand2);
    window.caret = XCreateFontCursor(window.display, XC_xterm);
    window.surface = cairo_xlib_surface_create(
        window.display, window.handle, DefaultVisual(window.display, screen), window.width, window.height);

    XMapWindow(window.display, window.handle);
    draw(&window);
    XSync(window.display, False);
    puts("first paint complete");
    fflush(stdout);

    if (!smoke_test) {
        app_action(ACTION_RESTORE);
    }

    bool running = !smoke_test;

    while (running) {
        bool dirty = false;

        while (XPending(window.display)) {
            XEvent event;
            XNextEvent(window.display, &event);

            if (XFilterEvent(&event, None)) {
                continue;
            }

            switch (event.type) {
            case MapNotify:
                window.visible = true;
                break;
            case UnmapNotify:
                window.visible = false;
                break;
            case Expose:
                dirty = true;
                break;
            case ConfigureNotify:
                window.width = event.xconfigure.width;
                window.height = event.xconfigure.height;
                cairo_xlib_surface_set_size(window.surface, window.width, window.height);
                dirty = true;
                break;
            case ClientMessage:
                if ((Atom)event.xclient.data.l[0] == window.close) {
                    running = false;
                }
                break;
            case FocusIn:
                window.focused = true;
                input_focus(&window);
                break;
            case FocusOut:
                window.focused = false;
                input_focus(&window);
                break;
            case LeaveNotify:
                ui_controller_leave(&window.controller, milliseconds());
                dirty = true;
                break;
            case ButtonPress:
                if (event.xbutton.button == 4 || event.xbutton.button == 5) {
                    ui_controller_scroll(&window.controller, (float)event.xbutton.x, (float)event.xbutton.y,
                                         event.xbutton.button == 4 ? -1 : 1, milliseconds());
                }

                if (event.xbutton.button == 3) {
                    ui_controller_context_press(&window.controller, (float)event.xbutton.x,
                                                (float)event.xbutton.y);
                }

                if (event.xbutton.button == 1 &&
                    ui_controller_press(&window.controller, (float)event.xbutton.x, (float)event.xbutton.y,
                                        milliseconds())) {
                    XGrabPointer(window.display, window.handle, False, PointerMotionMask | ButtonReleaseMask,
                                 GrabModeAsync, GrabModeAsync, None, None, event.xbutton.time);
                }

                input_focus(&window);
                dirty = true;
                break;
            case ButtonRelease:
                if (event.xbutton.button == 1) {
                    ui_controller_release(&window.controller, (float)event.xbutton.x, (float)event.xbutton.y,
                                          milliseconds());
                    XUngrabPointer(window.display, event.xbutton.time);
                    dirty = true;
                }
                break;
            case MotionNotify: {
                bool cursor_changed = false;
                bool changed = ui_controller_motion(&window.controller, (float)event.xmotion.x,
                                                    (float)event.xmotion.y, milliseconds(), &cursor_changed);

                if (cursor_changed) {
                    apply_cursor(&window);
                }

                dirty = dirty || changed;
                break;
            }
            case KeyPress:
                key(&window, &event.xkey);
                input_focus(&window);
                dirty = true;
                break;
            case SelectionNotify:
                paste(&window, &event.xselection);
                dirty = true;
                break;
            }
        }

        int sync_timeout = app_tick(window.visible);
        char drain[64];

        if (read(notifications[0], drain, sizeof(drain)) > 0) {
            dirty = true;
        }

        if (window.controller.scene.animating) {
            ui_controller_tick(&window.controller, milliseconds());
            dirty = true;
        }

        if (dirty) {
            draw(&window);
        }

        if (!running) {
            break;
        }

        struct pollfd descriptors[] = {{ConnectionNumber(window.display), POLLIN, 0},
                                       {notifications[0], POLLIN, 0}};

        if (poll(descriptors, 2, window.controller.scene.animating ? 16 : sync_timeout) < 0 &&
            errno != EINTR) {
            break;
        }
    }

    if (!smoke_test) {
        app_shutdown();
        exit(0);
    }

    if (window.input) {
        XDestroyIC(window.input);
    }

    if (method) {
        XCloseIM(method);
    }

    active_window = NULL;
    linux_canvas_destroy(&window.backend);
    cairo_surface_destroy(window.surface);
    cairo_surface_destroy(window.buffer);
    XDestroyWindow(window.display, window.handle);
    XCloseDisplay(window.display);
    close(notifications[0]);
    close(notifications[1]);

    return 0;
}
