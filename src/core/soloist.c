#include "soloist.h"
#include "artwork.h"
#include "platform.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_LIMIT (1024 * 1024)
#define REPLY_TIMEOUT_MS 5000
#define RECONNECT_MS 750
#define CHANGE_INTERVAL_MS 400

typedef struct {
    Listener socket;
    unsigned generation;
    unsigned char buffer[4096];
    size_t start;
    size_t end;
} Connection;

typedef enum { REPLY_OK, REPLY_REFUSED, REPLY_LOST, REPLY_UNREACHABLE } Reply;

/* Guarded by platform_lock(). */
static unsigned port;
static unsigned generation;
static bool active;
static void (*changed)(void);

static bool current(unsigned expected) {
    platform_lock();
    bool ok = expected == generation && port != 0;
    platform_unlock();
    return ok;
}

/* Reads more bytes, waiting in short slices so a detached session stops promptly. */
static bool fill(Connection *connection, uint64_t deadline_ms) {
    if (connection->start == connection->end) {
        connection->start = 0;
        connection->end = 0;
    } else if (connection->end == sizeof(connection->buffer)) {
        memmove(connection->buffer, connection->buffer + connection->start, connection->end - connection->start);
        connection->end -= connection->start;
        connection->start = 0;
    }

    for (;;) {
        if (!current(connection->generation)) {
            return false;
        }

        uint64_t now = platform_milliseconds();

        if (deadline_ms && now >= deadline_ms) {
            return false;
        }

        unsigned wait = deadline_ms && deadline_ms - now < 250 ? (unsigned)(deadline_ms - now) : 250;
        int count = loopback_receive(connection->socket, connection->buffer + connection->end,
                                     sizeof(connection->buffer) - connection->end, wait);

        if (count < 0) {
            return false;
        }

        if (count > 0) {
            connection->end += (size_t)count;
            return true;
        }
    }
}

static bool read_exact(Connection *connection, void *out, size_t size, uint64_t deadline_ms) {
    unsigned char *bytes = out;

    while (size) {
        if (connection->start == connection->end && !fill(connection, deadline_ms)) {
            return false;
        }

        size_t available = connection->end - connection->start;
        size_t take = available < size ? available : size;
        memcpy(bytes, connection->buffer + connection->start, take);
        connection->start += take;
        bytes += take;
        size -= take;
    }

    return true;
}

static bool send_frame(Connection *connection, unsigned opcode, const char *data, size_t size) {
    unsigned char *frame = malloc(size + 14);

    if (!frame) {
        return false;
    }

    size_t header = 0;
    frame[header++] = (unsigned char)(0x80 | opcode);

    if (size < 126) {
        frame[header++] = (unsigned char)(0x80 | size);
    } else if (size <= 0xffff) {
        frame[header++] = 0x80 | 126;
        frame[header++] = (unsigned char)(size >> 8);
        frame[header++] = (unsigned char)size;
    } else {
        frame[header++] = 0x80 | 127;

        for (int shift = 56; shift >= 0; shift -= 8) {
            frame[header++] = (unsigned char)((uint64_t)size >> shift);
        }
    }

    // Clients must mask every frame they send.
    unsigned char mask[4] = {0};
    platform_random(mask, sizeof(mask));
    memcpy(frame + header, mask, sizeof(mask));
    header += sizeof(mask);

    for (size_t i = 0; i < size; ++i) {
        frame[header + i] = (unsigned char)data[i] ^ mask[i % 4];
    }

    bool ok = loopback_send(connection->socket, frame, header + size);
    free(frame);
    return ok;
}

static bool send_text(Connection *connection, const char *text) {
    return send_frame(connection, 0x1, text, strlen(text));
}

/* Returns the next whole text message, answering pings on the way; NULL when the connection ends. */
static char *receive_message(Connection *connection, uint64_t deadline_ms) {
    char *message = NULL;
    size_t length = 0;

    for (;;) {
        unsigned char head[2];

        if (!read_exact(connection, head, sizeof(head), deadline_ms)) {
            break;
        }

        unsigned opcode = head[0] & 0x0f;
        bool final = (head[0] & 0x80) != 0;
        uint64_t size = head[1] & 0x7f;

        if (size >= 126) {
            unsigned char extended[8];
            size_t bytes = size == 126 ? 2 : 8;

            if (!read_exact(connection, extended, bytes, deadline_ms)) {
                break;
            }

            size = 0;

            for (size_t i = 0; i < bytes; ++i) {
                size = size << 8 | extended[i];
            }
        }

        unsigned char mask[4] = {0};
        bool masked = (head[1] & 0x80) != 0;

        if (masked && !read_exact(connection, mask, sizeof(mask), deadline_ms)) {
            break;
        }

        if (size > MESSAGE_LIMIT - length) {
            break;
        }

        char *grown = realloc(message, length + (size_t)size + 1);

        if (!grown) {
            break;
        }

        message = grown;

        if (!read_exact(connection, message + length, (size_t)size, deadline_ms)) {
            break;
        }

        for (size_t i = 0; masked && i < size; ++i) {
            message[length + i] ^= (char)mask[i % 4];
        }

        if (opcode == 0x8) {
            break;
        }

        // Control frames may arrive between fragments; their payload is not part of the message.
        if (opcode == 0x9) {
            send_frame(connection, 0xA, message + length, (size_t)size);
            continue;
        }

        if (opcode == 0xA) {
            continue;
        }

        length += (size_t)size;

        if (final) {
            message[length] = 0;
            return message;
        }
    }

    free(message);
    return NULL;
}

static bool open_connection(Connection *connection, unsigned target, uint64_t deadline_ms) {
    connection->socket = loopback_connect(target);

    if (connection->socket == INVALID_LISTENER) {
        return false;
    }

    unsigned char nonce[16];
    char key[32];

    if (!platform_random(nonce, sizeof(nonce)) || !base64url(nonce, sizeof(nonce), key, sizeof(key) - 2)) {
        loopback_close(connection->socket);
        return false;
    }

    // The handshake key is plain base64 of 16 bytes, padded.
    for (char *c = key; *c; ++c) {
        *c = *c == '-' ? '+' : *c == '_' ? '/' : *c;
    }

    strcat(key, "==");
    char request[256];
    snprintf(request, sizeof(request),
             "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
             target, key);

    char response[1024] = {0};
    size_t length = 0;
    bool sent = loopback_send(connection->socket, request, strlen(request));

    while (sent && length < sizeof(response) - 1 && !strstr(response, "\r\n\r\n") &&
           read_exact(connection, response + length, 1, deadline_ms)) {
        ++length;
    }

    if (!strstr(response, "\r\n\r\n") || strncmp(response, "HTTP/1.1 101", 12) != 0) {
        loopback_close(connection->socket);
        return false;
    }

    return true;
}

static bool session(Connection *connection, unsigned *target) {
    platform_lock();
    connection->generation = generation;
    *target = port;
    platform_unlock();
    return *target != 0;
}

static void set_active(unsigned expected, bool value) {
    platform_lock();

    if (expected == generation) {
        active = value;
    }

    platform_unlock();
}

/* One request on its own connection. Replies to other requests and pushed events are skipped. */
static Reply request(const char *body, const char *command, const char *reply_type, yyjson_doc **out) {
    Connection connection = {0};
    unsigned target = 0;
    uint64_t deadline = platform_milliseconds() + REPLY_TIMEOUT_MS;

    if (!session(&connection, &target) || !open_connection(&connection, target, deadline)) {
        return REPLY_UNREACHABLE;
    }

    if (!send_text(&connection, body)) {
        loopback_close(connection.socket);
        return REPLY_LOST;
    }

    Reply reply = REPLY_LOST;

    for (char *message; reply == REPLY_LOST && (message = receive_message(&connection, deadline));) {
        yyjson_doc *doc = yyjson_read(message, strlen(message), 0);
        yyjson_val *root = yyjson_doc_get_root(doc);
        const char *type = yyjson_get_str(yyjson_obj_get(root, "type"));
        const char *name = yyjson_get_str(yyjson_obj_get(root, "command"));
        free(message);

        if (type && !strcmp(type, "error")) {
            reply = REPLY_REFUSED;
        } else if (type && reply_type && !strcmp(type, reply_type)) {
            reply = REPLY_OK;
        } else if (type && !reply_type && !strcmp(type, "command_result") && name && !strcmp(name, command)) {
            reply = yyjson_is_false(yyjson_obj_get(root, "success")) ? REPLY_REFUSED : REPLY_OK;
        }

        if (reply == REPLY_OK && out) {
            *out = doc;
            doc = NULL;
        }

        yyjson_doc_free(doc);
    }

    loopback_close(connection.socket);
    return reply;
}

EngineCommand soloist_command(const char *command, unsigned value) {
    platform_lock();
    bool ready = port != 0 && active;
    platform_unlock();

    if (!ready) {
        return ENGINE_COMMAND_UNAVAILABLE;
    }

    char name[32];
    size_t length = strcspn(command, ":");
    snprintf(name, sizeof(name), "%.*s", (int)(length < sizeof(name) ? length : sizeof(name) - 1), command);

    const char *wire = !strcmp(name, "play")             ? "play"
                       : !strcmp(name, "pause")          ? "pause"
                       : !strcmp(name, "next")           ? "skip_next"
                       : !strcmp(name, "previous")       ? "skip_prev"
                       : !strcmp(name, "seek")           ? "seek"
                       : !strcmp(name, "volume")         ? "set_volume"
                       : !strcmp(name, "shuffle")        ? "set_shuffle"
                       : !strcmp(name, "repeat_track")   ? "set_repeat_track"
                       : !strcmp(name, "repeat_context") ? "set_repeat_context"
                                                         : NULL;

    if (!wire) {
        return ENGINE_COMMAND_UNAVAILABLE;
    }

    char body[160];
    int written = snprintf(body, sizeof(body), "{\"type\":\"command\",\"command\":\"%s\"", wire);

    if (!strcmp(name, "seek")) {
        written += snprintf(body + written, sizeof(body) - written, ",\"position_ms\":%u", value);
    } else if (!strcmp(name, "volume")) {
        written += snprintf(body + written, sizeof(body) - written, ",\"volume\":%u", value > 100 ? 100 : value);
    } else if (!strcmp(name, "shuffle") || !strncmp(name, "repeat_", 7)) {
        written += snprintf(body + written, sizeof(body) - written, ",\"enabled\":%s", value ? "true" : "false");
    }

    snprintf(body + written, sizeof(body) - written, "}");

    switch (request(body, wire, NULL, NULL)) {
    case REPLY_OK:
        return ENGINE_COMMAND_ACCEPTED;
    case REPLY_REFUSED:
        return ENGINE_COMMAND_REJECTED;
    case REPLY_LOST:
        return ENGINE_COMMAND_UNCONFIRMED;
    default:
        return ENGINE_COMMAND_UNAVAILABLE;
    }
}

static bool parse_state(yyjson_val *state, const char *device_id, const char *device_name, uint64_t now_ms,
                        Playback *out);

bool soloist_playback(const char *device_id, const char *device_name, Playback *out) {
    platform_lock();
    unsigned expected = generation;
    bool ready = port != 0 && active;
    platform_unlock();

    yyjson_doc *doc = NULL;

    if (!ready ||
        request("{\"type\":\"command\",\"command\":\"get_state\"}", "get_state", "playback_state", &doc) !=
            REPLY_OK) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);

    // Another device took over: playback state belongs to the Web API again.
    if (yyjson_is_false(yyjson_obj_get(root, "is_active"))) {
        set_active(expected, false);
        yyjson_doc_free(doc);
        return false;
    }

    bool ok = parse_state(root, device_id, device_name, platform_unix_milliseconds(), out);
    yyjson_doc_free(doc);
    return ok;
}

static const char *text(yyjson_val *object, const char *key) {
    const char *value = yyjson_get_str(yyjson_obj_get(object, key));
    return value ? value : "";
}

static const char *uri_id(const char *uri) {
    const char *colon = strrchr(uri, ':');
    const char *id = colon ? colon + 1 : uri;
    return strlen(id) == 22 ? id : "";
}

static const char *entity_name(yyjson_val *entity) {
    return text(yyjson_obj_get(yyjson_obj_get(entity, "decorations"), "identity"), "name");
}

bool soloist_state_parse(const char *json, const char *device_id, const char *device_name, uint64_t now_ms,
                         Playback *out) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    bool ok = parse_state(yyjson_doc_get_root(doc), device_id, device_name, now_ms, out);
    yyjson_doc_free(doc);
    return ok;
}

static bool parse_state(yyjson_val *state, const char *device_id, const char *device_name, uint64_t now_ms,
                        Playback *out) {
    const char *status = text(state, "status");
    bool known = !strcmp(status, "playing") || !strcmp(status, "paused") || !strcmp(status, "buffering") ||
                 !strcmp(status, "idle");

    if (!known) {
        return false;
    }

    playback_reset(out);

    yyjson_val *item = yyjson_obj_get(state, "item");
    yyjson_val *details = yyjson_obj_get(item, "decorations");
    yyjson_val *album = yyjson_obj_get(yyjson_obj_get(details, "parent"), "entity");
    yyjson_val *artist = yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(details, "creators"), 0), "entity");
    yyjson_val *anchor = yyjson_obj_get(state, "position");
    yyjson_val *speed_value = yyjson_obj_get(anchor, "speed");
    double speed = yyjson_is_num(speed_value) ? yyjson_get_num(speed_value) : 0;
    yyjson_val *duration = yyjson_obj_get(yyjson_obj_get(details, "playback"), "duration_ms");

    out->playing = !strcmp(status, "playing") && anchor && speed > 0;
    out->buffering = !strcmp(status, "buffering") || (!strcmp(status, "playing") && !out->playing);
    copy_utf8(out->uri, sizeof(out->uri), text(item, "uri"));
    copy_utf8(out->context_uri, sizeof(out->context_uri), text(yyjson_obj_get(state, "context"), "uri"));

    const char *title = text(yyjson_obj_get(details, "identity"), "name");
    copy_utf8(out->title, sizeof(out->title), title[0] ? title : "Nothing playing");
    copy_utf8(out->artist, sizeof(out->artist), entity_name(artist));
    copy_utf8(out->artist_id, sizeof(out->artist_id), uri_id(text(artist, "uri")));
    copy_utf8(out->album, sizeof(out->album), entity_name(album));
    copy_utf8(out->album_id, sizeof(out->album_id), uri_id(text(album, "uri")));
    artwork_url_parse(yyjson_obj_get(yyjson_obj_get(details, "visual_identity"), "cover"), out->artwork);

    yyjson_val *options = yyjson_obj_get(state, "options");
    const char *repeat = text(options, "repeat");
    out->shuffle = yyjson_is_true(yyjson_obj_get(options, "shuffle"));
    out->repeat = !strcmp(repeat, "track")     ? REPEAT_TRACK
                  : !strcmp(repeat, "context") ? REPEAT_CONTEXT
                                               : REPEAT_OFF;

    copy_utf8(out->device_id, sizeof(out->device_id), device_id);
    copy_utf8(out->device, sizeof(out->device), device_name);
    out->has_device = device_id[0] != 0;
    out->restricted = false;

    yyjson_val *volume = yyjson_obj_get(state, "volume");
    out->supports_volume = volume != NULL;

    if (yyjson_is_uint(volume) && yyjson_get_uint(volume) <= 100) {
        out->volume_percent = (int)yyjson_get_uint(volume);
    }

    if (yyjson_is_uint(duration) && yyjson_get_uint(duration) <= INT32_MAX) {
        out->duration_ms = (unsigned)yyjson_get_uint(duration);
    }

    if (anchor) {
        uint64_t position = yyjson_get_uint(yyjson_obj_get(anchor, "position_ms"));
        uint64_t stamp = yyjson_get_uint(yyjson_obj_get(anchor, "timestamp_ms"));

        // The anchor dates the position; a playing track has moved on since.
        if (out->playing && stamp && now_ms > stamp) {
            position += (uint64_t)((double)(now_ms - stamp) * speed);
        }

        if (out->duration_ms && position > out->duration_ms) {
            position = out->duration_ms;
        }

        out->position_ms = position > INT32_MAX ? INT32_MAX : (unsigned)position;
        out->position_known = true;
        out->anchor_position_ms = (unsigned)yyjson_get_uint(yyjson_obj_get(anchor, "position_ms"));
        out->anchor_timestamp_ms = stamp;
    }

    yyjson_val *actions = yyjson_obj_get(state, "available_actions");

    if (yyjson_is_obj(actions)) {
        out->seeking_disallowed = !yyjson_obj_get(actions, "seek");
        out->skipping_next_disallowed = !yyjson_obj_get(actions, "skip_next");
        out->skipping_previous_disallowed = !yyjson_obj_get(actions, "skip_prev");
    }

    return true;
}

/* Tracks activity from an event and reports whether playback state may have changed. */
static bool observe(unsigned expected, const char *message) {
    yyjson_doc *doc = yyjson_read(message, strlen(message), 0);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *is_active = yyjson_obj_get(root, "is_active");
    const char *type = text(root, "type");

    if (yyjson_is_bool(is_active)) {
        set_active(expected, yyjson_get_bool(is_active));
    }

    static const char *const changes[] = {"auth_state",      "device_changed", "track_changed",
                                          "playback_changed", "context_changed", "options_changed",
                                          "queue_changed",    "position_sync",  "volume_changed",
                                          "playback_state"};
    bool change = false;

    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        change = change || !strcmp(type, changes[i]);
    }

    yyjson_doc_free(doc);
    return change;
}

static void notify_changed(unsigned expected) {
    platform_lock();
    void (*callback)(void) = expected == generation ? changed : NULL;
    platform_unlock();

    if (callback) {
        callback();
    }
}

/* Waits for the next frame to begin; changes are reported at most every CHANGE_INTERVAL_MS. */
static void listen_events(Connection *connection, unsigned expected) {
    uint64_t last = 0;
    bool pending = false;

    for (;;) {
        uint64_t now = platform_milliseconds();

        if (pending && now >= last + CHANGE_INTERVAL_MS) {
            pending = false;
            last = now;
            notify_changed(expected);
        }

        if (connection->start == connection->end) {
            unsigned wait = pending ? (unsigned)(last + CHANGE_INTERVAL_MS - now) : 1000;
            int count = loopback_receive(connection->socket, connection->buffer, sizeof(connection->buffer), wait);

            if (count < 0 || !current(expected)) {
                return;
            }

            connection->start = 0;
            connection->end = (size_t)count;
            continue;
        }

        char *message = receive_message(connection, 0);

        if (!message) {
            return;
        }

        bool change = observe(expected, message);
        free(message);
        now = platform_milliseconds();

        if (change && now >= last + CHANGE_INTERVAL_MS) {
            last = now;
            notify_changed(expected);
        } else if (change) {
            pending = true;
        }
    }
}

static void run_events(void *argument) {
    unsigned expected = (unsigned)(uintptr_t)argument;

    while (current(expected)) {
        Connection connection = {.generation = expected};

        platform_lock();
        unsigned target = port;
        platform_unlock();

        if (open_connection(&connection, target, platform_milliseconds() + REPLY_TIMEOUT_MS)) {
            // The reply tells us whether this device is the active one.
            if (send_text(&connection, "{\"type\":\"command\",\"command\":\"get_state\"}")) {
                listen_events(&connection, expected);
            }

            loopback_close(connection.socket);
            set_active(expected, false);
            notify_changed(expected);
        }

        if (current(expected)) {
            platform_sleep(RECONNECT_MS);
        }
    }
}

void soloist_attach(unsigned new_port, void (*callback)(void)) {
    platform_lock();
    unsigned expected = ++generation;
    port = new_port;
    active = false;
    changed = callback;
    platform_unlock();

    if (new_port) {
        platform_start_thread(run_events, (void *)(uintptr_t)expected);
    }
}

void soloist_detach(void) {
    platform_lock();
    ++generation;
    port = 0;
    active = false;
    platform_unlock();
}
