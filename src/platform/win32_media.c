#define COBJMACROS

#include "win32_media.h"

#include <inspectable.h>
#include <systemmediatransportcontrolsinterop.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Windows.Media ABI. MinGW ships no WinRT projection headers, so the few members used are declared here. */

enum { MEDIA_CLOSED = 0, MEDIA_PLAYING = 3, MEDIA_PAUSED = 4 };
enum { BUTTON_PLAY = 0, BUTTON_PAUSE = 1, BUTTON_STOP = 2, BUTTON_NEXT = 6, BUTTON_PREVIOUS = 7 };
enum { MEDIA_TYPE_MUSIC = 1 };

#define INSPECTABLE_METHODS                                                                                  \
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *self, REFIID riid, void **out);                         \
    ULONG(STDMETHODCALLTYPE *AddRef)(void *self);                                                            \
    ULONG(STDMETHODCALLTYPE *Release)(void *self);                                                           \
    void *GetIids;                                                                                           \
    void *GetRuntimeClassName;                                                                               \
    void *GetTrustLevel;

typedef struct {
    INSPECTABLE_METHODS
    void *get_PlaybackStatus;
    HRESULT(STDMETHODCALLTYPE *put_PlaybackStatus)(void *self, int status);
    HRESULT(STDMETHODCALLTYPE *get_DisplayUpdater)(void *self, void **updater);
    void *get_SoundLevel;
    void *get_IsEnabled;
    HRESULT(STDMETHODCALLTYPE *put_IsEnabled)(void *self, boolean value);
    void *get_IsPlayEnabled;
    HRESULT(STDMETHODCALLTYPE *put_IsPlayEnabled)(void *self, boolean value);
    void *get_IsStopEnabled;
    void *put_IsStopEnabled;
    void *get_IsPauseEnabled;
    HRESULT(STDMETHODCALLTYPE *put_IsPauseEnabled)(void *self, boolean value);
    void *record[2];
    void *fast_forward[2];
    void *rewind[2];
    void *get_IsPreviousEnabled;
    HRESULT(STDMETHODCALLTYPE *put_IsPreviousEnabled)(void *self, boolean value);
    void *get_IsNextEnabled;
    HRESULT(STDMETHODCALLTYPE *put_IsNextEnabled)(void *self, boolean value);
    void *channel[4];
    HRESULT(STDMETHODCALLTYPE *add_ButtonPressed)(void *self, void *handler, INT64 *token);
    HRESULT(STDMETHODCALLTYPE *remove_ButtonPressed)(void *self, INT64 token);
} ControlsMethods;

typedef struct {
    INSPECTABLE_METHODS
    void *get_Type;
    HRESULT(STDMETHODCALLTYPE *put_Type)(void *self, int type);
    void *get_AppMediaId;
    void *put_AppMediaId;
    void *get_Thumbnail;
    HRESULT(STDMETHODCALLTYPE *put_Thumbnail)(void *self, void *stream);
    HRESULT(STDMETHODCALLTYPE *get_MusicProperties)(void *self, void **music);
    void *get_VideoProperties;
    void *get_ImageProperties;
    void *CopyFromFileAsync;
    HRESULT(STDMETHODCALLTYPE *ClearAll)(void *self);
    HRESULT(STDMETHODCALLTYPE *Update)(void *self);
} UpdaterMethods;

typedef struct {
    INSPECTABLE_METHODS
    void *get_Title;
    HRESULT(STDMETHODCALLTYPE *put_Title)(void *self, HSTRING value);
    void *get_AlbumArtist;
    void *put_AlbumArtist;
    void *get_Artist;
    HRESULT(STDMETHODCALLTYPE *put_Artist)(void *self, HSTRING value);
} MusicMethods;

typedef struct {
    INSPECTABLE_METHODS
    void *get_AlbumTitle;
    HRESULT(STDMETHODCALLTYPE *put_AlbumTitle)(void *self, HSTRING value);
} AlbumMethods;

typedef struct {
    INSPECTABLE_METHODS
    HRESULT(STDMETHODCALLTYPE *get_Button)(void *self, int *button);
} ButtonArgsMethods;

typedef struct {
    INSPECTABLE_METHODS
    HRESULT(STDMETHODCALLTYPE *CreateUri)(void *self, HSTRING text, void **uri);
} UriFactoryMethods;

typedef struct {
    INSPECTABLE_METHODS
    void *CreateFromFile;
    HRESULT(STDMETHODCALLTYPE *CreateFromUri)(void *self, void *uri, void **stream);
} StreamStaticsMethods;

typedef struct {
    const ControlsMethods *methods;
} Controls;

typedef struct {
    const UpdaterMethods *methods;
} Updater;

typedef struct {
    const MusicMethods *methods;
} Music;

typedef struct {
    const AlbumMethods *methods;
} Album;

typedef struct {
    const ButtonArgsMethods *methods;
} ButtonArgs;

typedef struct {
    const UriFactoryMethods *methods;
} UriFactory;

typedef struct {
    const StreamStaticsMethods *methods;
} StreamStatics;

typedef struct {
    INSPECTABLE_METHODS
} UnknownMethods;

typedef struct {
    const UnknownMethods *methods;
} Unknown;

static const IID IID_Controls = {
    0x99fa3ff4, 0x1742, 0x42a6, {0x90, 0x2e, 0x08, 0x7d, 0x41, 0xf9, 0x65, 0xec}};
static const IID IID_Album = {0x00368462, 0x97d3, 0x44b9, {0xb0, 0x0f, 0x00, 0x8a, 0xfc, 0xef, 0xaf, 0x18}};
static const IID IID_UriFactory = {
    0x44a9796f, 0x723e, 0x4fdf, {0xa2, 0x18, 0x03, 0x3e, 0x75, 0xb0, 0xc0, 0x84}};
static const IID IID_StreamStatics = {
    0x857309dc, 0x3fbf, 0x4e7d, {0x98, 0x6f, 0xef, 0x3b, 0x1a, 0x07, 0xa9, 0x64}};
static const IID IID_Interop = {0xddb0472d, 0xc911, 0x4a1f, {0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a}};
static const IID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const IID IID_Unknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* TypedEventHandler<SystemMediaTransportControls, SystemMediaTransportControlsButtonPressedEventArgs>. */
static const IID IID_ButtonHandler = {
    0x0557e996, 0x7b23, 0x5bae, {0xaa, 0x81, 0xea, 0x0d, 0x67, 0x11, 0x43, 0xa4}};

typedef HRESULT(WINAPI *ActivationFactory)(HSTRING name, REFIID riid, void **factory);
typedef HRESULT(WINAPI *CreateString)(LPCWSTR text, UINT32 length, HSTRING *string);
typedef HRESULT(WINAPI *DeleteString)(HSTRING string);

typedef struct {
    bool enabled;
    int status;
    bool previous;
    bool next;
    char title[512];
    char artist[512];
    char album[512];
    char artwork[ARTWORK_URL_CAPACITY];
} MediaState;

static HWND target;
static UINT button_message;

static HMODULE runtime;
static ActivationFactory activation_factory;
static CreateString create_string;
static DeleteString delete_string;

static Controls *controls;
static INT64 button_token;
static MediaState shown;

static void post_action(Action action) {
    PostMessageW(target, button_message, (WPARAM)action, 0);
}

static HRESULT STDMETHODCALLTYPE handler_query(void *self, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_ButtonHandler) || IsEqualIID(riid, &IID_Unknown) ||
        IsEqualIID(riid, &IID_Agile)) {
        *out = self;
        return S_OK;
    }

    *out = NULL;
    return E_NOINTERFACE;
}

/* The handler is a static object, so reference counting has nothing to free. */
static ULONG STDMETHODCALLTYPE handler_add_ref(void *self) {
    (void)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE handler_release(void *self) {
    (void)self;
    return 1;
}

/* Runs on a system thread; the window thread performs the action. */
static HRESULT STDMETHODCALLTYPE handler_invoke(void *self, void *sender, ButtonArgs *args) {
    (void)self;
    (void)sender;
    int button = -1;

    if (!args || FAILED(args->methods->get_Button(args, &button))) {
        return S_OK;
    }

    if (button == BUTTON_PLAY) {
        post_action(ACTION_PLAY);
    }

    if (button == BUTTON_PAUSE || button == BUTTON_STOP) {
        post_action(ACTION_PAUSE);
    }

    if (button == BUTTON_NEXT) {
        post_action(ACTION_NEXT);
    }

    if (button == BUTTON_PREVIOUS) {
        post_action(ACTION_PREVIOUS);
    }

    return S_OK;
}

static const struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *self, REFIID riid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(void *self);
    ULONG(STDMETHODCALLTYPE *Release)(void *self);
    HRESULT(STDMETHODCALLTYPE *Invoke)(void *self, void *sender, ButtonArgs *args);
} handler_methods = {handler_query, handler_add_ref, handler_release, handler_invoke};

static struct {
    const void *methods;
} button_handler = {&handler_methods};

static void release(void *object) {
    if (object) {
        ((Unknown *)object)->methods->Release(object);
    }
}

static bool load_runtime(void) {
    runtime = LoadLibraryExW(L"combase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!runtime) {
        return false;
    }

    activation_factory = (ActivationFactory)(void (*)(void))GetProcAddress(runtime, "RoGetActivationFactory");
    create_string = (CreateString)(void (*)(void))GetProcAddress(runtime, "WindowsCreateString");
    delete_string = (DeleteString)(void (*)(void))GetProcAddress(runtime, "WindowsDeleteString");

    if (!activation_factory || !create_string || !delete_string) {
        FreeLibrary(runtime);
        runtime = NULL;
        return false;
    }

    return true;
}

static HSTRING wide_string(const wchar_t *text) {
    HSTRING string = NULL;

    if (FAILED(create_string(text, (UINT32)wcslen(text), &string))) {
        return NULL;
    }

    return string;
}

static HSTRING utf8_string(const char *text) {
    wchar_t wide[1024];

    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, ARRAYSIZE(wide)) <= 0) {
        return NULL;
    }

    return wide_string(wide);
}

static void *factory(const wchar_t *name, const IID *iid) {
    HSTRING class_name = wide_string(name);
    void *result = NULL;

    if (!class_name) {
        return NULL;
    }

    if (FAILED(activation_factory(class_name, iid, &result))) {
        result = NULL;
    }

    delete_string(class_name);
    return result;
}

static bool start_controls(void) {
    ISystemMediaTransportControlsInterop *interop =
        load_runtime() ? factory(L"Windows.Media.SystemMediaTransportControls", &IID_Interop) : NULL;

    if (!interop) {
        return false;
    }

    void *created = NULL;
    HRESULT result =
        ISystemMediaTransportControlsInterop_GetForWindow(interop, target, &IID_Controls, &created);
    ISystemMediaTransportControlsInterop_Release(interop);

    if (FAILED(result) || !created) {
        return false;
    }

    controls = created;

    if (FAILED(controls->methods->add_ButtonPressed(controls, &button_handler, &button_token))) {
        release(controls);
        controls = NULL;
        return false;
    }

    controls->methods->put_IsEnabled(controls, FALSE);
    return true;
}

bool win32_media_start(HWND window, UINT button) {
    target = window;
    button_message = button;
    memset(&shown, 0, sizeof(shown));
    return start_controls();
}

bool win32_media_active(void) {
    return controls != NULL;
}

static void apply_controls(const MediaState *state) {
    controls->methods->put_IsEnabled(controls, state->enabled);
    controls->methods->put_IsPlayEnabled(controls, state->enabled);
    controls->methods->put_IsPauseEnabled(controls, state->enabled);
    controls->methods->put_IsPreviousEnabled(controls, state->previous);
    controls->methods->put_IsNextEnabled(controls, state->next);
    controls->methods->put_PlaybackStatus(controls, state->status);
}

static void set_thumbnail(Updater *updater, const char *url) {
    if (!url[0]) {
        updater->methods->put_Thumbnail(updater, NULL);
        return;
    }

    UriFactory *uris = factory(L"Windows.Foundation.Uri", &IID_UriFactory);
    StreamStatics *streams =
        factory(L"Windows.Storage.Streams.RandomAccessStreamReference", &IID_StreamStatics);
    HSTRING text = utf8_string(url);
    void *uri = NULL;
    void *stream = NULL;

    if (uris && streams && text && SUCCEEDED(uris->methods->CreateUri(uris, text, &uri)) &&
        SUCCEEDED(streams->methods->CreateFromUri(streams, uri, &stream))) {
        updater->methods->put_Thumbnail(updater, stream);
    }

    if (text) {
        delete_string(text);
    }

    release(stream);
    release(uri);
    release(streams);
    release(uris);
}

static void set_text(HRESULT(STDMETHODCALLTYPE *put)(void *self, HSTRING value), void *object,
                     const char *text) {
    HSTRING value = utf8_string(text);

    if (!value) {
        return;
    }

    put(object, value);
    delete_string(value);
}

static void apply_metadata(const MediaState *state) {
    void *created = NULL;

    if (FAILED(controls->methods->get_DisplayUpdater(controls, &created)) || !created) {
        return;
    }

    Updater *updater = created;

    if (!state->enabled) {
        updater->methods->ClearAll(updater);
        updater->methods->Update(updater);
        release(updater);
        return;
    }

    updater->methods->put_Type(updater, MEDIA_TYPE_MUSIC);
    void *properties = NULL;

    if (SUCCEEDED(updater->methods->get_MusicProperties(updater, &properties)) && properties) {
        Music *music = properties;
        void *album = NULL;

        set_text(music->methods->put_Title, music, state->title);
        set_text(music->methods->put_Artist, music, state->artist);

        if (SUCCEEDED(music->methods->QueryInterface(music, &IID_Album, &album)) && album) {
            set_text(((Album *)album)->methods->put_AlbumTitle, album, state->album);
            release(album);
        }

        release(music);
    }

    set_thumbnail(updater, state->artwork);
    updater->methods->Update(updater);
    release(updater);
}

void win32_media_update(const View *view) {
    if (!controls || !view) {
        return;
    }

    const Playback *playback = &view->playback;
    MediaState next;
    memset(&next, 0, sizeof(next));

    next.enabled = view->authenticated && playback->title[0];
    next.status = !next.enabled ? MEDIA_CLOSED : playback->playing ? MEDIA_PLAYING : MEDIA_PAUSED;
    next.previous = next.enabled && playback_can_command(playback, "previous");
    next.next = next.enabled && playback_can_command(playback, "next");

    if (next.enabled) {
        snprintf(next.title, sizeof(next.title), "%s", playback->title);
        snprintf(next.artist, sizeof(next.artist), "%s", playback->artist);
        snprintf(next.album, sizeof(next.album), "%s", playback->album);
        snprintf(next.artwork, sizeof(next.artwork), "%s", playback->artwork);
    }

    bool controls_changed = next.enabled != shown.enabled || next.status != shown.status ||
                            next.previous != shown.previous || next.next != shown.next;
    bool metadata_changed = next.enabled != shown.enabled || strcmp(next.title, shown.title) ||
                            strcmp(next.artist, shown.artist) || strcmp(next.album, shown.album) ||
                            strcmp(next.artwork, shown.artwork);

    if (controls_changed) {
        apply_controls(&next);
    }

    if (metadata_changed) {
        apply_metadata(&next);
    }

    shown = next;
}

void win32_media_stop(void) {
    if (controls) {
        controls->methods->put_IsEnabled(controls, FALSE);
        controls->methods->remove_ButtonPressed(controls, button_token);
        release(controls);
        controls = NULL;
    }

    if (runtime) {
        FreeLibrary(runtime);
        runtime = NULL;
    }
}
