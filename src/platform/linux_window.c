#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "canvas.h"
#include "controller.h"
#include "linux_canvas.h"
#include "ui.h"

#include <X11/XKBlib.h>
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
    Atom clipboard, utf8, targets, paste, key_paste, close;
    Cursor hand, caret, resize;
    bool focused, input_active, visible;
    bool lyrics_fullscreen;
    bool space_down;
    int width, height;
    char clipboard_text[SEARCH_CAPACITY];
    size_t clipboard_size;
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

    if (cursor == UI_CURSOR_RESIZE) {
        XDefineCursor(window->display, window->handle, window->resize);
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
    float scale = scene_view_scale(window->width, window->height, 1.0f);
    cairo_scale(paint, scale, scale);
    Canvas canvas;
    linux_canvas_begin(&window->backend, paint, &canvas);
    ui_controller_draw(&window->controller, window->width / scale, window->height / scale, &canvas);
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
    case XK_Delete:
    case XK_KP_Delete:
        *mapped = KEY_DELETE;
        return true;
    case XK_Left:
    case XK_KP_Left:
        *mapped = KEY_LEFT;
        return true;
    case XK_Right:
    case XK_KP_Right:
        *mapped = KEY_RIGHT;
        return true;
    case XK_Up:
    case XK_KP_Up:
        *mapped = KEY_UP;
        return true;
    case XK_Down:
    case XK_KP_Down:
        *mapped = KEY_DOWN;
        return true;
    case XK_Home:
    case XK_KP_Home:
        *mapped = KEY_HOME;
        return true;
    case XK_End:
    case XK_KP_End:
        *mapped = KEY_END;
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

static void copy_text(NativeWindow *window, Time when, const char *utf8) {
    size_t length = strlen(utf8);

    if (length >= sizeof(window->clipboard_text)) {
        length = sizeof(window->clipboard_text) - 1;
    }

    memcpy(window->clipboard_text, utf8, length);
    window->clipboard_text[length] = 0;
    window->clipboard_size = length;
    XSetSelectionOwner(window->display, window->clipboard, window->handle, when);
    XFlush(window->display);
}

static void respond_selection(NativeWindow *window, XSelectionRequestEvent *request) {
    XEvent reply = {0};
    reply.xselection.type = SelectionNotify;
    reply.xselection.requestor = request->requestor;
    reply.xselection.selection = request->selection;
    reply.xselection.target = request->target;
    reply.xselection.time = request->time;
    reply.xselection.property = None;

    if (request->selection == window->clipboard && window->clipboard_size > 0 && request->property != None) {
        if (request->target == window->targets) {
            Atom supported[] = {window->targets, window->utf8, XA_STRING};
            XChangeProperty(window->display, request->requestor, request->property, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)supported, 3);
            reply.xselection.property = request->property;
        } else if (request->target == window->utf8 || request->target == XA_STRING) {
            XChangeProperty(window->display, request->requestor, request->property, request->target, 8,
                            PropModeReplace, (const unsigned char *)window->clipboard_text,
                            (int)window->clipboard_size);
            reply.xselection.property = request->property;
        }
    }

    XSendEvent(window->display, request->requestor, False, 0, &reply);
    XFlush(window->display);
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
    bool shift = (event->state & ShiftMask) != 0;

    if (ctrl && ((symbol >= XK_a && symbol <= XK_z) || (symbol >= XK_A && symbol <= XK_Z))) {
        UiShortcutResult shortcut = ui_controller_shortcut(&window->controller, (char)symbol, true);

        if (shortcut == UI_SHORTCUT_PASTE_SEARCH) {
            if (XGetSelectionOwner(window->display, window->clipboard) == window->handle &&
                window->clipboard_size > 0) {
                ui_controller_text(&window->controller, window->clipboard_text, window->clipboard_size);
                return;
            }

            XConvertSelection(window->display, window->clipboard, window->utf8, window->paste, window->handle,
                              event->time);
            return;
        }

        if (shortcut == UI_SHORTCUT_COPY_SEARCH || shortcut == UI_SHORTCUT_CUT_SEARCH) {
            char selected[SEARCH_CAPACITY];

            if (ui_controller_copy(&window->controller, selected, sizeof(selected),
                                   shortcut == UI_SHORTCUT_CUT_SEARCH)) {
                copy_text(window, event->time, selected);
            }

            return;
        }

        if (shortcut == UI_SHORTCUT_HANDLED) {
            return;
        }
    }

    // Holding Space must not toggle playback over and over.
    if (symbol == XK_space && !ui_controller_is_editing(&window->controller)) {
        if (window->space_down) {
            return;
        }

        window->space_down = true;
    }

    SceneKey mapped;

    if (mapped_key(symbol, &mapped) &&
        (symbol != XK_space || !ui_controller_is_editing(&window->controller))) {
        ui_controller_key_mods(&window->controller, mapped, shift, ctrl, milliseconds());
        return;
    }

    if (ctrl && ui_controller_is_editing(&window->controller)) {
        return;
    }

    if (!ctrl && size > 0 && ui_controller_is_editing(&window->controller)) {
        ui_controller_text(&window->controller, text, (size_t)size);
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
            size_t len = count;

            while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) {
                --len;
            }

            if (len > 0) {
                ui_controller_text(&window->controller, (const char *)data, len);
            }
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

    NativeWindow *window = calloc(1, sizeof(*window));

    if (!window) {
        return 1;
    }

    window->width = 1280;
    window->height = 800;
    window->visible = true;
    ui_controller_init(&window->controller, milliseconds());
    window->display = XOpenDisplay(NULL);

    if (!window->display) {
        fprintf(stderr, "No X11/XWayland display.\n");
        ui_controller_destroy(&window->controller);
        free(window);
        return 1;
    }

    if (pipe(notifications)) {
        XCloseDisplay(window->display);
        ui_controller_destroy(&window->controller);
        free(window);
        return 1;
    }

    fcntl(notifications[0], F_SETFL, O_NONBLOCK);
    fcntl(notifications[1], F_SETFL, O_NONBLOCK);

    int screen = DefaultScreen(window->display);
    window->handle = XCreateSimpleWindow(window->display, RootWindow(window->display, screen), 0, 0,
                                         (unsigned)window->width, (unsigned)window->height, 0, 0, 0);

    window->close = XInternAtom(window->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(window->display, window->handle, &window->close, 1);
    XStoreName(window->display, window->handle, "Native Spotify");
    XClassHint class_hint = {.res_name = "native_spotify", .res_class = "Native Spotify"};
    XSetClassHint(window->display, window->handle, &class_hint);
    XSizeHints hints = {.flags = PMinSize, .min_width = 960, .min_height = 640};
    XSetWMNormalHints(window->display, window->handle, &hints);

    static const unsigned icon_sizes[] = {16, 32, 48, 128};
    size_t icon_words = 0;

    for (unsigned i = 0; i < sizeof(icon_sizes) / sizeof(icon_sizes[0]); ++i) {
        icon_words += 2 + (size_t)icon_sizes[i] * icon_sizes[i];
    }

    unsigned long *icon_prop = malloc(icon_words * sizeof(*icon_prop));

    if (icon_prop) {
        size_t pos = 0;

        for (unsigned i = 0; i < sizeof(icon_sizes) / sizeof(icon_sizes[0]); ++i) {
            unsigned side = icon_sizes[i];
            icon_prop[pos++] = side;
            icon_prop[pos++] = side;
            uint32_t *pixels = malloc((size_t)side * side * sizeof(*pixels));

            if (pixels) {
                canvas_brand_icon_pixels(side, pixels);

                for (size_t p = 0; p < (size_t)side * side; ++p) {
                    icon_prop[pos++] = (unsigned long)pixels[p];
                }

                free(pixels);
            } else {
                pos += (size_t)side * side;
            }
        }

        Atom net_wm_icon = XInternAtom(window->display, "_NET_WM_ICON", False);
        XChangeProperty(window->display, window->handle, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)icon_prop, (int)pos);
        free(icon_prop);
    }

    XIM method = XOpenIM(window->display, NULL, NULL, NULL);

    if (method) {
        window->input = XCreateIC(method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                                  window->handle, XNFocusWindow, window->handle, NULL);
    }

    long filter = 0;

    if (window->input) {
        XGetICValues(window->input, XNFilterEvents, &filter, NULL);
    }

    XSelectInput(window->display, window->handle,
                 ExposureMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | KeyPressMask | KeyReleaseMask | FocusChangeMask | LeaveWindowMask |
                     filter);

    // Held keys then repeat as presses only, so a real release marks the end of a hold.
    XkbSetDetectableAutoRepeat(window->display, True, NULL);

    window->clipboard = XInternAtom(window->display, "CLIPBOARD", False);
    window->utf8 = XInternAtom(window->display, "UTF8_STRING", False);
    window->targets = XInternAtom(window->display, "TARGETS", False);
    window->paste = XInternAtom(window->display, "NATIVE_PASTE", False);
    window->key_paste = XInternAtom(window->display, "NATIVE_KEY_PASTE", False);
    active_window = window;
    window->hand = XCreateFontCursor(window->display, XC_hand2);
    window->caret = XCreateFontCursor(window->display, XC_xterm);
    window->resize = XCreateFontCursor(window->display, XC_sb_h_double_arrow);
    window->surface =
        cairo_xlib_surface_create(window->display, window->handle, DefaultVisual(window->display, screen),
                                  window->width, window->height);

    XMapWindow(window->display, window->handle);
    draw(window);
    XSync(window->display, False);
    puts("first paint complete");
    fflush(stdout);

    if (!smoke_test) {
        app_action(ACTION_RESTORE);
    }

    bool running = !smoke_test;

    while (running) {
        bool dirty = false;

        while (XPending(window->display)) {
            XEvent event;
            XNextEvent(window->display, &event);

            if (XFilterEvent(&event, None)) {
                continue;
            }

            switch (event.type) {
            case MapNotify:
                window->visible = true;
                break;
            case UnmapNotify:
                window->visible = false;
                break;
            case Expose:
                dirty = true;
                break;
            case ConfigureNotify:
                window->width = event.xconfigure.width;
                window->height = event.xconfigure.height;
                cairo_xlib_surface_set_size(window->surface, window->width, window->height);
                dirty = true;
                break;
            case ClientMessage:
                if ((Atom)event.xclient.data.l[0] == window->close) {
                    running = false;
                }
                break;
            case FocusIn:
                window->focused = true;
                input_focus(window);
                break;
            case FocusOut:
                window->focused = false;
                window->space_down = false;
                input_focus(window);
                break;
            case KeyRelease:
                if (XLookupKeysym(&event.xkey, 0) == XK_space) {
                    window->space_down = false;
                }
                break;
            case LeaveNotify:
                ui_controller_leave(&window->controller, milliseconds());
                dirty = true;
                break;
            case ButtonPress: {
                float scale = scene_view_scale(window->width, window->height, 1.0f);
                float x = event.xbutton.x / scale;
                float y = event.xbutton.y / scale;

                if (event.xbutton.button == 4 || event.xbutton.button == 5) {
                    ui_controller_scroll(&window->controller, x, y, event.xbutton.button == 4 ? -1 : 1,
                                         milliseconds());
                }

                if (event.xbutton.button == 3) {
                    ui_controller_context_press(&window->controller, x, y);
                }

                if (event.xbutton.button == 1 &&
                    ui_controller_press(&window->controller, x, y, milliseconds())) {
                    XGrabPointer(window->display, window->handle, False,
                                 PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None,
                                 None, event.xbutton.time);
                }

                input_focus(window);
                dirty = true;
                break;
            }
            case ButtonRelease:
                if (event.xbutton.button == 1) {
                    float scale = scene_view_scale(window->width, window->height, 1.0f);
                    ui_controller_release(&window->controller, event.xbutton.x / scale,
                                          event.xbutton.y / scale, milliseconds());
                    XUngrabPointer(window->display, event.xbutton.time);
                    dirty = true;
                }
                break;
            case MotionNotify: {
                bool cursor_changed = false;
                float scale = scene_view_scale(window->width, window->height, 1.0f);
                bool changed = ui_controller_motion(&window->controller, event.xmotion.x / scale,
                                                    event.xmotion.y / scale, milliseconds(), &cursor_changed);

                if (cursor_changed) {
                    apply_cursor(window);
                }

                dirty = dirty || changed;
                break;
            }
            case KeyPress:
                key(window, &event.xkey);
                input_focus(window);
                dirty = true;
                break;
            case SelectionNotify:
                paste(window, &event.xselection);
                dirty = true;
                break;
            case SelectionRequest:
                respond_selection(window, &event.xselectionrequest);
                break;
            }
        }

        if (window->lyrics_fullscreen != window->controller.scene.lyrics_fullscreen) {
            window->lyrics_fullscreen = window->controller.scene.lyrics_fullscreen;
            XEvent request = {0};
            request.xclient.type = ClientMessage;
            request.xclient.window = window->handle;
            request.xclient.message_type = XInternAtom(window->display, "_NET_WM_STATE", False);
            request.xclient.format = 32;
            request.xclient.data.l[0] = window->lyrics_fullscreen ? 1 : 0;
            request.xclient.data.l[1] = (long)XInternAtom(window->display, "_NET_WM_STATE_FULLSCREEN", False);
            request.xclient.data.l[3] = 1;
            XSendEvent(window->display, DefaultRootWindow(window->display), False,
                       SubstructureRedirectMask | SubstructureNotifyMask, &request);
            XFlush(window->display);
            dirty = true;
        }

        int sync_timeout = app_tick(window->visible);
        char drain[64];

        if (read(notifications[0], drain, sizeof(drain)) > 0) {
            dirty = true;
        }

        if (window->controller.scene.animating) {
            ui_controller_tick(&window->controller, milliseconds());
            dirty = true;
        }

        if (dirty) {
            draw(window);
        }

        if (!running) {
            break;
        }

        struct pollfd descriptors[] = {{ConnectionNumber(window->display), POLLIN, 0},
                                       {notifications[0], POLLIN, 0}};

        if (poll(descriptors, 2, window->controller.scene.animating ? 16 : sync_timeout) < 0 &&
            errno != EINTR) {
            break;
        }
    }

    if (!smoke_test) {
        app_shutdown();
        ui_controller_destroy(&window->controller);
        free(window);
        exit(0);
    }

    if (window->input) {
        XDestroyIC(window->input);
    }

    if (method) {
        XCloseIM(method);
    }

    active_window = NULL;
    linux_canvas_destroy(&window->backend);
    cairo_surface_destroy(window->surface);
    cairo_surface_destroy(window->buffer);
    XDestroyWindow(window->display, window->handle);
    XCloseDisplay(window->display);
    close(notifications[0]);
    close(notifications[1]);
    ui_controller_destroy(&window->controller);
    free(window);

    return 0;
}
