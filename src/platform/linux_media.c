#define _POSIX_C_SOURCE 200809L
#include "linux_media.h"
#include "platform.h"
#include "ui.h"

#include <gio/gio.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MPRIS_BUS_NAME "org.mpris.MediaPlayer2.native_spotify"
#define MPRIS_OBJECT_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT_IFACE "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define MPRIS_NO_TRACK_PATH "/org/mpris/MediaPlayer2/TrackList/NoTrack"

static const char mpris_xml[] =
    "<node>"
    "  <interface name='org.mpris.MediaPlayer2'>"
    "    <method name='Raise'/>"
    "    <method name='Quit'/>"
    "    <property name='CanQuit' type='b' access='read'/>"
    "    <property name='CanRaise' type='b' access='read'/>"
    "    <property name='HasTrackList' type='b' access='read'/>"
    "    <property name='Identity' type='s' access='read'/>"
    "    <property name='DesktopEntry' type='s' access='read'/>"
    "    <property name='SupportedUriSchemes' type='as' access='read'/>"
    "    <property name='SupportedMimeTypes' type='as' access='read'/>"
    "  </interface>"
    "  <interface name='org.mpris.MediaPlayer2.Player'>"
    "    <method name='Next'/>"
    "    <method name='Previous'/>"
    "    <method name='Pause'/>"
    "    <method name='PlayPause'/>"
    "    <method name='Stop'/>"
    "    <method name='Play'/>"
    "    <method name='Seek'>"
    "      <arg direction='in' name='Offset' type='x'/>"
    "    </method>"
    "    <method name='SetPosition'>"
    "      <arg direction='in' name='TrackId' type='o'/>"
    "      <arg direction='in' name='Position' type='x'/>"
    "    </method>"
    "    <method name='OpenUri'>"
    "      <arg direction='in' name='Uri' type='s'/>"
    "    </method>"
    "    <signal name='Seeked'>"
    "      <arg name='Position' type='x'/>"
    "    </signal>"
    "    <property name='PlaybackStatus' type='s' access='read'/>"
    "    <property name='LoopStatus' type='s' access='readwrite'/>"
    "    <property name='Rate' type='d' access='readwrite'/>"
    "    <property name='Shuffle' type='b' access='readwrite'/>"
    "    <property name='Metadata' type='a{sv}' access='read'/>"
    "    <property name='Volume' type='d' access='readwrite'/>"
    "    <property name='Position' type='x' access='read'/>"
    "    <property name='MinimumRate' type='d' access='read'/>"
    "    <property name='MaximumRate' type='d' access='read'/>"
    "    <property name='CanGoNext' type='b' access='read'/>"
    "    <property name='CanGoPrevious' type='b' access='read'/>"
    "    <property name='CanPlay' type='b' access='read'/>"
    "    <property name='CanPause' type='b' access='read'/>"
    "    <property name='CanSeek' type='b' access='read'/>"
    "    <property name='CanControl' type='b' access='read'/>"
    "  </interface>"
    "</node>";

typedef struct {
    bool enabled;
    bool playing;
    bool previous;
    bool next;
    bool can_seek;
    bool shuffle;
    RepeatMode repeat;
    int volume_percent;
    unsigned position_ms;
    unsigned duration_ms;
    uint64_t position_updated_ms;
    char uri[256];
    char track_path[160];
    char title[512];
    char artist[512];
    char album[512];
    char artwork[ARTWORK_URL_CAPACITY];
} MediaState;

static pthread_mutex_t media_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t media_ready = PTHREAD_COND_INITIALIZER;
static pthread_t media_thread;
static bool thread_running;
static bool init_done;
static bool media_started;
static bool raise_pending;

static GMainContext *media_context;
static GMainLoop *media_loop;
static GDBusConnection *media_bus;
static GDBusNodeInfo *media_introspection;
static guint root_reg_id;
static guint player_reg_id;
static guint bus_name_id;

static MediaState current_state;
static MediaState published_state;

static void build_track_path(const char *uri, char out[160]) {
    if (!uri || !uri[0]) {
        snprintf(out, 160, "%s", MPRIS_NO_TRACK_PATH);
        return;
    }

    const char *prefix = "/org/mpris/MediaPlayer2/track/";
    size_t pos = strlen(prefix);
    memcpy(out, prefix, pos);

    for (size_t i = 0; uri[i] && pos + 1 < 160; ++i) {
        unsigned char c = (unsigned char)uri[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out[pos++] = ok ? (char)c : '_';
    }

    if (pos == strlen(prefix)) {
        snprintf(out, 160, "/org/mpris/MediaPlayer2/track/current");
        return;
    }

    out[pos] = 0;
}

static const char *playback_status_str(const MediaState *state) {
    if (!state->enabled) {
        return "Stopped";
    }

    return state->playing ? "Playing" : "Paused";
}

static const char *loop_status_str(RepeatMode repeat) {
    if (repeat == REPEAT_TRACK) {
        return "Track";
    }

    if (repeat == REPEAT_CONTEXT) {
        return "Playlist";
    }

    return "None";
}

static int64_t interpolated_position_us(const MediaState *state) {
    if (!state->enabled) {
        return 0;
    }

    uint64_t pos_ms = state->position_ms;

    if (state->playing && state->position_updated_ms > 0) {
        uint64_t now = platform_milliseconds();

        if (now > state->position_updated_ms) {
            pos_ms += now - state->position_updated_ms;
        }
    }

    if (state->duration_ms > 0 && pos_ms > state->duration_ms) {
        pos_ms = state->duration_ms;
    }

    return (int64_t)pos_ms * 1000;
}

static GVariant *build_metadata(const MediaState *state) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

    if (!state->enabled) {
        g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
                              g_variant_new_object_path(MPRIS_NO_TRACK_PATH));
        return g_variant_builder_end(&builder);
    }

    const char *path = state->track_path[0] ? state->track_path : "/org/mpris/MediaPlayer2/track/current";
    g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(path));

    if (state->duration_ms > 0) {
        g_variant_builder_add(&builder, "{sv}", "mpris:length",
                              g_variant_new_int64((gint64)state->duration_ms * 1000));
    }

    if (state->artwork[0]) {
        g_variant_builder_add(&builder, "{sv}", "mpris:artUrl", g_variant_new_string(state->artwork));
    }

    if (state->title[0]) {
        g_variant_builder_add(&builder, "{sv}", "xesam:title", g_variant_new_string(state->title));
    }

    if (state->artist[0]) {
        GVariantBuilder artists;
        g_variant_builder_init(&artists, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&artists, "s", state->artist);
        g_variant_builder_add(&builder, "{sv}", "xesam:artist", g_variant_builder_end(&artists));
    }

    if (state->album[0]) {
        g_variant_builder_add(&builder, "{sv}", "xesam:album", g_variant_new_string(state->album));
    }

    if (!strncmp(state->uri, "spotify:track:", 14) && spotify_id_valid(state->uri + 14)) {
        char url[96];
        snprintf(url, sizeof(url), "https://open.spotify.com/track/%s", state->uri + 14);
        g_variant_builder_add(&builder, "{sv}", "xesam:url", g_variant_new_string(url));
    }

    return g_variant_builder_end(&builder);
}

static void handle_root_method(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                               const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;

    if (!strcmp(method_name, "Raise")) {
        pthread_mutex_lock(&media_lock);
        raise_pending = true;
        pthread_mutex_unlock(&media_lock);
        ui_wake();
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *handle_root_get(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                 const gchar *interface_name, const gchar *property_name, GError **error,
                                 gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;

    if (!strcmp(property_name, "CanQuit")) {
        return g_variant_new_boolean(FALSE);
    }

    if (!strcmp(property_name, "CanRaise")) {
        return g_variant_new_boolean(TRUE);
    }

    if (!strcmp(property_name, "HasTrackList")) {
        return g_variant_new_boolean(FALSE);
    }

    if (!strcmp(property_name, "Identity")) {
        return g_variant_new_string("Native Spotify");
    }

    if (!strcmp(property_name, "DesktopEntry")) {
        return g_variant_new_string("native_spotify");
    }

    if (!strcmp(property_name, "SupportedUriSchemes") || !strcmp(property_name, "SupportedMimeTypes")) {
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE("as"));
        return g_variant_builder_end(&empty);
    }

    return NULL;
}

static void handle_player_method(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                 const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                                 GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (!strcmp(method_name, "Play")) {
        app_action(ACTION_PLAY);
    } else if (!strcmp(method_name, "Pause") || !strcmp(method_name, "Stop")) {
        app_action(ACTION_PAUSE);
    } else if (!strcmp(method_name, "PlayPause")) {
        app_action(ACTION_TOGGLE_PLAYBACK);
    } else if (!strcmp(method_name, "Next")) {
        app_action(ACTION_NEXT);
    } else if (!strcmp(method_name, "Previous")) {
        app_action(ACTION_PREVIOUS);
    } else if (!strcmp(method_name, "Seek")) {
        gint64 offset_us = 0;
        g_variant_get(parameters, "(x)", &offset_us);

        pthread_mutex_lock(&media_lock);
        MediaState snapshot = current_state;
        pthread_mutex_unlock(&media_lock);

        if (snapshot.enabled && snapshot.can_seek && snapshot.duration_ms > 0) {
            int64_t target_us = interpolated_position_us(&snapshot) + offset_us;

            if (target_us < 0) {
                target_us = 0;
            }

            uint64_t target_ms = (uint64_t)(target_us / 1000);

            if (target_ms >= snapshot.duration_ms) {
                app_action(ACTION_NEXT);
            } else {
                app_seek_lyric(snapshot.uri, (unsigned)target_ms);
            }
        }
    } else if (!strcmp(method_name, "SetPosition")) {
        const gchar *track_id = NULL;
        gint64 position_us = 0;
        g_variant_get(parameters, "(&ox)", &track_id, &position_us);

        pthread_mutex_lock(&media_lock);
        MediaState snapshot = current_state;
        pthread_mutex_unlock(&media_lock);

        if (snapshot.enabled && snapshot.can_seek && track_id && !strcmp(track_id, snapshot.track_path) &&
            position_us >= 0 && snapshot.duration_ms > 0) {
            uint64_t target_ms = (uint64_t)(position_us / 1000);

            if (target_ms < snapshot.duration_ms) {
                app_seek_lyric(snapshot.uri, (unsigned)target_ms);
            }
        }
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *handle_player_get(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                   const gchar *interface_name, const gchar *property_name, GError **error,
                                   gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;

    pthread_mutex_lock(&media_lock);
    MediaState snapshot = current_state;
    pthread_mutex_unlock(&media_lock);

    if (!strcmp(property_name, "PlaybackStatus")) {
        return g_variant_new_string(playback_status_str(&snapshot));
    }

    if (!strcmp(property_name, "LoopStatus")) {
        return g_variant_new_string(loop_status_str(snapshot.repeat));
    }

    if (!strcmp(property_name, "Rate") || !strcmp(property_name, "MinimumRate") ||
        !strcmp(property_name, "MaximumRate")) {
        return g_variant_new_double(1.0);
    }

    if (!strcmp(property_name, "Shuffle")) {
        return g_variant_new_boolean(snapshot.shuffle);
    }

    if (!strcmp(property_name, "Metadata")) {
        return build_metadata(&snapshot);
    }

    if (!strcmp(property_name, "Volume")) {
        double vol = snapshot.volume_percent >= 0 ? (double)snapshot.volume_percent / 100.0 : 1.0;
        return g_variant_new_double(vol);
    }

    if (!strcmp(property_name, "Position")) {
        return g_variant_new_int64(interpolated_position_us(&snapshot));
    }

    if (!strcmp(property_name, "CanGoNext")) {
        return g_variant_new_boolean(snapshot.next);
    }

    if (!strcmp(property_name, "CanGoPrevious")) {
        return g_variant_new_boolean(snapshot.previous);
    }

    if (!strcmp(property_name, "CanPlay")) {
        return g_variant_new_boolean(snapshot.enabled);
    }

    if (!strcmp(property_name, "CanPause")) {
        return g_variant_new_boolean(snapshot.enabled);
    }

    if (!strcmp(property_name, "CanSeek")) {
        return g_variant_new_boolean(snapshot.can_seek);
    }

    if (!strcmp(property_name, "CanControl")) {
        return g_variant_new_boolean(TRUE);
    }

    return NULL;
}

static gboolean handle_player_set(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                  const gchar *interface_name, const gchar *property_name, GVariant *value,
                                  GError **error, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;

    if (!strcmp(property_name, "Volume")) {
        double vol = g_variant_get_double(value);

        if (vol < 0.0) {
            vol = 0.0;
        }

        if (vol > 1.0) {
            vol = 1.0;
        }

        app_volume((unsigned)(vol * 100.0 + 0.5));
        return TRUE;
    }

    if (!strcmp(property_name, "Shuffle")) {
        bool desired = g_variant_get_boolean(value) != FALSE;

        pthread_mutex_lock(&media_lock);
        bool current = current_state.shuffle;
        pthread_mutex_unlock(&media_lock);

        if (desired != current) {
            app_action(ACTION_SHUFFLE);
        }

        return TRUE;
    }

    if (!strcmp(property_name, "LoopStatus")) {
        const gchar *desired = g_variant_get_string(value, NULL);

        pthread_mutex_lock(&media_lock);
        const char *current = loop_status_str(current_state.repeat);
        pthread_mutex_unlock(&media_lock);

        if (desired && strcmp(desired, current)) {
            app_action(ACTION_REPEAT);
        }

        return TRUE;
    }

    return TRUE;
}

static const GDBusInterfaceVTable root_vtable = {
    .method_call = handle_root_method,
    .get_property = handle_root_get,
    .set_property = NULL,
};

static const GDBusInterfaceVTable player_vtable = {
    .method_call = handle_player_method,
    .get_property = handle_player_get,
    .set_property = handle_player_set,
};

static gboolean emit_changes_on_thread(gpointer user_data) {
    (void)user_data;

    if (!media_bus) {
        return G_SOURCE_REMOVE;
    }

    pthread_mutex_lock(&media_lock);
    MediaState next = current_state;
    pthread_mutex_unlock(&media_lock);

    bool status_changed = next.enabled != published_state.enabled || next.playing != published_state.playing;
    bool prev_changed = next.previous != published_state.previous;
    bool next_changed = next.next != published_state.next;
    bool seek_cap_changed = next.can_seek != published_state.can_seek;
    bool play_cap_changed = next.enabled != published_state.enabled;
    bool shuffle_changed = next.shuffle != published_state.shuffle;
    bool loop_changed = next.repeat != published_state.repeat;
    bool volume_changed = next.volume_percent != published_state.volume_percent;
    bool metadata_changed = next.enabled != published_state.enabled ||
                            next.duration_ms != published_state.duration_ms ||
                            strcmp(next.uri, published_state.uri) != 0 ||
                            strcmp(next.title, published_state.title) != 0 ||
                            strcmp(next.artist, published_state.artist) != 0 ||
                            strcmp(next.album, published_state.album) != 0 ||
                            strcmp(next.artwork, published_state.artwork) != 0;

    bool any_changed = status_changed || prev_changed || next_changed || seek_cap_changed ||
                       play_cap_changed || shuffle_changed || loop_changed || volume_changed ||
                       metadata_changed;

    if (any_changed) {
        GVariantBuilder changed;
        GVariantBuilder invalidated;
        g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

        if (status_changed) {
            g_variant_builder_add(&changed, "{sv}", "PlaybackStatus",
                                  g_variant_new_string(playback_status_str(&next)));
        }

        if (metadata_changed) {
            g_variant_builder_add(&changed, "{sv}", "Metadata", build_metadata(&next));
        }

        if (play_cap_changed) {
            g_variant_builder_add(&changed, "{sv}", "CanPlay", g_variant_new_boolean(next.enabled));
            g_variant_builder_add(&changed, "{sv}", "CanPause", g_variant_new_boolean(next.enabled));
        }

        if (prev_changed) {
            g_variant_builder_add(&changed, "{sv}", "CanGoPrevious", g_variant_new_boolean(next.previous));
        }

        if (next_changed) {
            g_variant_builder_add(&changed, "{sv}", "CanGoNext", g_variant_new_boolean(next.next));
        }

        if (seek_cap_changed) {
            g_variant_builder_add(&changed, "{sv}", "CanSeek", g_variant_new_boolean(next.can_seek));
        }

        if (shuffle_changed) {
            g_variant_builder_add(&changed, "{sv}", "Shuffle", g_variant_new_boolean(next.shuffle));
        }

        if (loop_changed) {
            g_variant_builder_add(&changed, "{sv}", "LoopStatus",
                                  g_variant_new_string(loop_status_str(next.repeat)));
        }

        if (volume_changed && next.volume_percent >= 0) {
            g_variant_builder_add(&changed, "{sv}", "Volume",
                                  g_variant_new_double((double)next.volume_percent / 100.0));
        }

        g_dbus_connection_emit_signal(media_bus, NULL, MPRIS_OBJECT_PATH, "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)", MPRIS_PLAYER_IFACE, &changed, &invalidated),
                                      NULL);
    }

    bool same_track = next.enabled && published_state.enabled && !strcmp(next.uri, published_state.uri);

    if (same_track) {
        int64_t expected_us = interpolated_position_us(&published_state);
        int64_t actual_us = (int64_t)next.position_ms * 1000;
        int64_t diff_us = actual_us > expected_us ? actual_us - expected_us : expected_us - actual_us;

        if (diff_us > 2000000) {
            g_dbus_connection_emit_signal(media_bus, NULL, MPRIS_OBJECT_PATH, MPRIS_PLAYER_IFACE, "Seeked",
                                          g_variant_new("(x)", actual_us), NULL);
        }
    }

    published_state = next;
    return G_SOURCE_REMOVE;
}

static void *media_thread_main(void *argument) {
    (void)argument;

    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

    if (error) {
        g_error_free(error);
        error = NULL;
    }

    GDBusNodeInfo *introspection = bus ? g_dbus_node_info_new_for_xml(mpris_xml, &error) : NULL;

    if (error) {
        g_error_free(error);
        error = NULL;
    }

    guint root_id = 0;
    guint player_id = 0;
    guint name_id = 0;

    if (bus && introspection) {
        root_id = g_dbus_connection_register_object(bus, MPRIS_OBJECT_PATH, introspection->interfaces[0],
                                                    &root_vtable, NULL, NULL, NULL);
        player_id = g_dbus_connection_register_object(bus, MPRIS_OBJECT_PATH, introspection->interfaces[1],
                                                      &player_vtable, NULL, NULL, NULL);

        if (root_id && player_id) {
            name_id = g_bus_own_name_on_connection(bus, MPRIS_BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
                                                   NULL, NULL, NULL);
        }
    }

    bool ok = bus && introspection && root_id && player_id && name_id;
    GMainLoop *loop = ok ? g_main_loop_new(context, FALSE) : NULL;

    pthread_mutex_lock(&media_lock);
    media_context = ok ? context : NULL;
    media_loop = loop;
    media_bus = ok ? bus : NULL;
    media_introspection = ok ? introspection : NULL;
    root_reg_id = root_id;
    player_reg_id = player_id;
    bus_name_id = name_id;
    media_started = ok;
    init_done = true;
    pthread_cond_signal(&media_ready);
    pthread_mutex_unlock(&media_lock);

    if (ok && loop) {
        g_main_loop_run(loop);
    }

    if (name_id) {
        g_bus_unown_name(name_id);
    }

    if (bus && player_id) {
        g_dbus_connection_unregister_object(bus, player_id);
    }

    if (bus && root_id) {
        g_dbus_connection_unregister_object(bus, root_id);
    }

    if (bus) {
        g_dbus_connection_flush_sync(bus, NULL, NULL);
        g_object_unref(bus);
    }

    if (introspection) {
        g_dbus_node_info_unref(introspection);
    }

    if (loop) {
        g_main_loop_unref(loop);
    }

    media_bus = NULL;
    media_introspection = NULL;
    root_reg_id = 0;
    player_reg_id = 0;
    bus_name_id = 0;

    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return NULL;
}

bool linux_media_start(void) {
    if (getenv("NATIVE_CONTROLLER_OFFLINE")) {
        return false;
    }

    pthread_mutex_lock(&media_lock);

    if (thread_running) {
        bool started = media_started;
        pthread_mutex_unlock(&media_lock);
        return started;
    }

    memset(&current_state, 0, sizeof(current_state));
    memset(&published_state, 0, sizeof(published_state));
    current_state.volume_percent = -1;
    published_state.volume_percent = -1;
    raise_pending = false;
    init_done = false;
    media_started = false;

    if (pthread_create(&media_thread, NULL, media_thread_main, NULL) != 0) {
        pthread_mutex_unlock(&media_lock);
        return false;
    }

    thread_running = true;

    while (!init_done) {
        pthread_cond_wait(&media_ready, &media_lock);
    }

    bool started = media_started;
    pthread_mutex_unlock(&media_lock);

    if (!started) {
        pthread_join(media_thread, NULL);
        pthread_mutex_lock(&media_lock);
        thread_running = false;
        pthread_mutex_unlock(&media_lock);
    }

    return started;
}

bool linux_media_active(void) {
    pthread_mutex_lock(&media_lock);
    bool active = media_started;
    pthread_mutex_unlock(&media_lock);
    return active;
}

void linux_media_update(const View *view) {
    if (!view) {
        return;
    }

    pthread_mutex_lock(&media_lock);

    if (!media_started || !media_context) {
        pthread_mutex_unlock(&media_lock);
        return;
    }

    const Playback *playback = &view->playback;
    MediaState next;
    memset(&next, 0, sizeof(next));
    next.volume_percent = -1;

    next.enabled = view->authenticated && playback->title[0];
    next.playing = next.enabled && playback->playing;
    next.previous = next.enabled && playback_can_command(playback, "previous");
    next.next = next.enabled && playback_can_command(playback, "next");
    next.can_seek = next.enabled && playback_can_seek(playback);
    next.shuffle = next.enabled && playback->shuffle;
    next.repeat = next.enabled ? playback->repeat : REPEAT_OFF;
    next.volume_percent = next.enabled ? playback->volume_percent : -1;
    next.position_ms = next.enabled ? playback->position_ms : 0;
    next.duration_ms = next.enabled ? playback->duration_ms : 0;
    next.position_updated_ms = next.enabled ? platform_milliseconds() : 0;

    if (next.enabled) {
        snprintf(next.uri, sizeof(next.uri), "%s", playback->uri);
        build_track_path(playback->uri, next.track_path);
        snprintf(next.title, sizeof(next.title), "%s", playback->title);
        snprintf(next.artist, sizeof(next.artist), "%s", playback->artist);
        snprintf(next.album, sizeof(next.album), "%s", playback->album);
        snprintf(next.artwork, sizeof(next.artwork), "%s", playback->artwork);
    } else {
        snprintf(next.track_path, sizeof(next.track_path), "%s", MPRIS_NO_TRACK_PATH);
    }

    bool changed = next.enabled != current_state.enabled || next.playing != current_state.playing ||
                   next.previous != current_state.previous || next.next != current_state.next ||
                   next.can_seek != current_state.can_seek || next.shuffle != current_state.shuffle ||
                   next.repeat != current_state.repeat ||
                   next.volume_percent != current_state.volume_percent ||
                   next.duration_ms != current_state.duration_ms ||
                   strcmp(next.uri, current_state.uri) != 0 ||
                   strcmp(next.title, current_state.title) != 0 ||
                   strcmp(next.artist, current_state.artist) != 0 ||
                   strcmp(next.album, current_state.album) != 0 ||
                   strcmp(next.artwork, current_state.artwork) != 0;

    bool position_jumped = false;

    if (next.enabled && current_state.enabled && !strcmp(next.uri, current_state.uri)) {
        int64_t expected_us = interpolated_position_us(&current_state);
        int64_t actual_us = (int64_t)next.position_ms * 1000;
        int64_t diff_us = actual_us > expected_us ? actual_us - expected_us : expected_us - actual_us;
        position_jumped = diff_us > 2000000;
    }

    current_state = next;
    GMainContext *context = (changed || position_jumped) ? g_main_context_ref(media_context) : NULL;
    pthread_mutex_unlock(&media_lock);

    if (context) {
        g_main_context_invoke(context, emit_changes_on_thread, NULL);
        g_main_context_unref(context);
    }
}

bool linux_media_take_raise(void) {
    pthread_mutex_lock(&media_lock);
    bool pending = raise_pending;
    raise_pending = false;
    pthread_mutex_unlock(&media_lock);
    return pending;
}

void linux_media_stop(void) {
    pthread_mutex_lock(&media_lock);

    if (!thread_running) {
        pthread_mutex_unlock(&media_lock);
        return;
    }

    GMainLoop *loop = media_loop ? g_main_loop_ref(media_loop) : NULL;
    media_started = false;
    media_context = NULL;
    media_loop = NULL;
    pthread_mutex_unlock(&media_lock);

    if (loop) {
        g_main_loop_quit(loop);
        g_main_loop_unref(loop);
    }

    pthread_join(media_thread, NULL);

    pthread_mutex_lock(&media_lock);
    thread_running = false;
    pthread_mutex_unlock(&media_lock);
}
