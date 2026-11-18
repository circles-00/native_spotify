#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "ui.h"
#include "scene.h"
#include "linux_canvas.h"

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
    Scene scene;
    LinuxCanvas backend;
} NativeWindow;

static uint64_t milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void input_focus(NativeWindow *window) {
    bool active = window->focused && !window->scene.login_screen && window->scene.focus == HIT_SEARCH;
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

static void draw(NativeWindow *window) {
    View view = app_snapshot();
    artwork_reset(view.session_revision);
    scene_layout(&window->scene, &view, (float)window->width, (float)window->height);

    if (window->scene.first_row < view.list.first ||
        (window->scene.first_row + ROW_SLOTS > view.list.first + view.list.rows_count &&
         view.list.first + view.list.rows_count < view.list.count) ||
        window->scene.library_first < view.playlists.first ||
        (window->scene.library_first + 24 > view.playlists.first + view.playlists.rows_count &&
         view.playlists.first + view.playlists.rows_count < view.playlists.count)) {
        view = app_snapshot();
    }
    input_focus(window);

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
    scene_draw(&window->scene, &view, &canvas);
    linux_canvas_end(&window->backend);

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

    if ((event->state & ControlMask) && symbol == XK_l) {
        if (window->scene.login_screen) {
            return;
        }
        window->scene.focus = HIT_SEARCH;
        window->scene.select_all = true;
        return;
    }

    if (window->scene.setup_popup && (event->state & ControlMask) && symbol == XK_v) {
        ui_paste_engine_key();
        return;
    }

    bool editor = !window->scene.login_screen && window->scene.focus == HIT_SEARCH;

    if (editor && (event->state & ControlMask)) {
        if (symbol == XK_a) {
            window->scene.select_all = true;
        }
        if (symbol == XK_u) {
            window->scene.query[0] = 0;
            scene_query_changed(&window->scene);
        }
        if (symbol == XK_v) {
            XConvertSelection(window->display, window->clipboard, window->utf8, window->paste, window->handle,
                              event->time);
        }
        return;
    }

    if (editor && size > 0 && symbol != XK_Return && symbol != XK_Tab && symbol != XK_BackSpace &&
        symbol != XK_Escape && !(event->state & ControlMask)) {
        scene_input(&window->scene, text, (size_t)size);
        return;
    }

    SceneKey mapped;

    switch (symbol) {
    case XK_Tab:
    case XK_ISO_Left_Tab:
        mapped = KEY_TAB;
        break;
    case XK_Return:
    case XK_KP_Enter:
        mapped = KEY_ENTER;
        break;
    case XK_BackSpace:
        mapped = KEY_BACKSPACE;
        break;
    case XK_Left:
        mapped = KEY_LEFT;
        break;
    case XK_Right:
        mapped = KEY_RIGHT;
        break;
    case XK_Up:
        mapped = KEY_UP;
        break;
    case XK_Down:
        mapped = KEY_DOWN;
        break;
    case XK_Escape:
        mapped = KEY_ESCAPE;
        break;
    case XK_space:
        mapped = KEY_SPACE;
        break;
    default:
        return;
    }

    View view = app_snapshot();
    scene_key(&window->scene, &view, mapped, (event->state & ShiftMask) != 0);
    if (window->scene.entrance == 0) {
        window->scene.last_ms = milliseconds();
    }
}

static NativeWindow *active_window;

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

    if (event->property != window->paste || window->scene.focus != HIT_SEARCH || window->scene.login_screen) {
        return;
    }

    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;

    if (XGetWindowProperty(window->display, window->handle, window->paste, 0, SEARCH_CAPACITY, True,
                           window->utf8, &type, &format, &count, &remaining, &data) == Success) {
        if (type == window->utf8 && format == 8 && !remaining && data) {
            scene_input(&window->scene, (const char *)data, count);
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
    scene_init(&window.scene);
    window.scene.last_ms = milliseconds();
    window.display = XOpenDisplay(NULL);
    if (!window.display) {
        fprintf(stderr, "No X11/XWayland display.\n");
        return 1;
    }
    if (pipe(notifications)) {
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
                scene_hover(&window.scene, HIT_NONE, milliseconds());
                dirty = true;
                break;
            case ButtonPress: {
                View view = app_snapshot();

                if (event.xbutton.button == 4 || event.xbutton.button == 5) {
                    uint64_t now = milliseconds();
                    if (window.scene.animating) {
                        scene_tick(&window.scene, now);
                    }
                    window.scene.last_ms = now;
                    window.scene.hot =
                        scene_hit(&window.scene, (float)event.xbutton.x, (float)event.xbutton.y);
                    window.scene.over_panel =
                        window.scene.panel != PANEL_NONE && event.xbutton.x >= window.scene.side_panel.x &&
                        event.xbutton.y >= window.scene.side_panel.y &&
                        event.xbutton.y < window.scene.side_panel.y + window.scene.side_panel.h;
                    window.scene.over_library =
                        event.xbutton.x < window.scene.sidebar.x + window.scene.sidebar.w &&
                        event.xbutton.y >= window.scene.sidebar.y && event.xbutton.y < window.scene.player.y;
                    scene_scroll(&window.scene, &view, event.xbutton.button == 4 ? -1 : 1);
                }

                if (event.xbutton.button == 3) {
                    scene_context_press(&window.scene, &view, (float)event.xbutton.x, (float)event.xbutton.y);
                }

                if (event.xbutton.button == 1) {
                    scene_press(&window.scene, &view, (float)event.xbutton.x, (float)event.xbutton.y,
                                milliseconds());
                    if (window.scene.dragging != HIT_NONE) {
                        XGrabPointer(window.display, window.handle, False,
                                     PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                                     None, None, event.xbutton.time);
                    }
                }

                input_focus(&window);
                dirty = true;
                break;
            }
            case ButtonRelease:
                if (event.xbutton.button == 1) {
                    View view = app_snapshot();
                    scene_pointer_motion(&window.scene, (float)event.xbutton.x, (float)event.xbutton.y);
                    scene_release(&window.scene, &view, (float)event.xbutton.x, (float)event.xbutton.y);
                    XUngrabPointer(window.display, event.xbutton.time);
                    if (window.scene.entrance == 0) {
                        window.scene.last_ms = milliseconds();
                    }
                    dirty = true;
                }
                break;
            case MotionNotify: {
                HitId target = scene_hit(&window.scene, (float)event.xmotion.x, (float)event.xmotion.y);
                bool changed = target != window.scene.hot || window.scene.dragging != HIT_NONE;
                if (window.scene.dragging != HIT_NONE) {
                    scene_pointer_motion(&window.scene, (float)event.xmotion.x, (float)event.xmotion.y);
                }
                scene_hover(&window.scene, target, milliseconds());

                if (changed && target == HIT_SEARCH) {
                    XDefineCursor(window.display, window.handle, window.caret);
                }
                if (changed && target != HIT_SEARCH && target != HIT_NONE) {
                    XDefineCursor(window.display, window.handle, window.hand);
                }
                if (changed && target == HIT_NONE) {
                    XUndefineCursor(window.display, window.handle);
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
        if (window.scene.animating) {
            scene_tick(&window.scene, milliseconds());
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
        if (poll(descriptors, 2, window.scene.animating ? 16 : sync_timeout) < 0 && errno != EINTR) {
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
