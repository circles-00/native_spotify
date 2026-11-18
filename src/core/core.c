#include "core.h"
#include "platform.h"
#include "yyjson.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

bool base64url(const unsigned char *data, size_t size, char *out, size_t capacity) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    if (size > (SIZE_MAX - 2) / 4 || capacity <= (size * 4 + 2) / 3) {
        return false;
    }
    size_t written = 0;
    unsigned accumulator = 0;
    unsigned bits = 0;

    for (size_t i = 0; i < size; ++i) {
        accumulator = (accumulator << 8) | data[i];
        bits += 8;

        while (bits >= 6) {
            bits -= 6;
            out[written++] = alphabet[(accumulator >> bits) & 63];
        }
    }

    if (bits) {
        out[written++] = alphabet[(accumulator << (6 - bits)) & 63];
    }
    out[written] = 0;

    return true;
}

bool url_encode(const char *text, char *out, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t written = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                    strchr("-._~", *p);

        if (capacity - written <= (safe ? 1u : 3u)) {
            return false;
        }

        if (safe) {
            out[written++] = (char)*p;
        }

        if (!safe) {
            out[written++] = '%';
            out[written++] = hex[*p >> 4];
            out[written++] = hex[*p & 15];
        }
    }

    if (written >= capacity) {
        return false;
    }
    out[written] = 0;

    return true;
}

bool pkce_create(Pkce *pkce) {
    unsigned char random[32] = {0};
    unsigned char digest[32] = {0};

    memset(pkce, 0, sizeof(*pkce));

    bool ok = platform_random(random, sizeof(random)) &&
              base64url(random, sizeof(random), pkce->verifier, sizeof(pkce->verifier)) &&
              platform_sha256(pkce->verifier, digest) &&
              base64url(digest, sizeof(digest), pkce->challenge, sizeof(pkce->challenge)) &&
              platform_random(random, sizeof(random)) &&
              base64url(random, sizeof(random), pkce->state, sizeof(pkce->state));

    platform_wipe(random, sizeof(random));
    platform_wipe(digest, sizeof(digest));

    return ok;
}

bool client_id_valid(const char *client_id) {
    if (!client_id || strlen(client_id) != 32) {
        return false;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (!isxdigit((unsigned char)client_id[i])) {
            return false;
        }
    }
    return true;
}

bool authorization_url(const char *client_id, const Pkce *pkce, char *out, size_t capacity) {
    char redirect[128];
    char scope[512];

    if (!client_id_valid(client_id) || !url_encode(REDIRECT_URI, redirect, sizeof(redirect)) ||
        !url_encode(AUTH_SCOPE, scope, sizeof(scope))) {
        return false;
    }

    int size = snprintf(out, capacity,
                        "https://accounts.spotify.com/authorize?response_type=code&client_id=%s"
                        "&redirect_uri=%s&scope=%s&code_challenge_method=S256&code_challenge=%s&state=%s",
                        client_id, redirect, scope, pkce->challenge, pkce->state);

    return size > 0 && (size_t)size < capacity;
}

static int unhex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    return -1;
}

static bool query_decode(const char *start, size_t length, char *out, size_t capacity) {
    size_t written = 0;

    for (size_t i = 0; i < length; ++i) {
        unsigned char c = start[i] == '+' ? ' ' : (unsigned char)start[i];

        if (c == '%') {
            if (i + 2 >= length || unhex(start[i + 1]) < 0 || unhex(start[i + 2]) < 0) {
                return false;
            }

            c = (unsigned char)((unhex(start[i + 1]) << 4) | unhex(start[i + 2]));
            i += 2;
        }

        if (c < 32 || c == 127 || written + 1 >= capacity) {
            return false;
        }
        out[written++] = (char)c;
    }

    if (!capacity) {
        return false;
    }
    out[written] = 0;

    return true;
}

CallbackResult callback_result(const char *request, const char *state, char *code, size_t capacity) {
    if (!capacity) {
        return CALLBACK_INVALID;
    }

    code[0] = 0;

    if (strncmp(request, "GET /callback?", 14)) {
        return CALLBACK_INVALID;
    }

    const char *end = strstr(request + 14, " HTTP/1.1\r\n");

    if (!end) {
        return CALLBACK_INVALID;
    }

    char received_state[128] = {0};
    bool has_state = false;
    bool has_code = false;
    bool denied = false;

    for (const char *p = request + 14; p < end;) {
        const char *next = memchr(p, '&', (size_t)(end - p));

        if (!next) {
            next = end;
        }

        const char *equal = memchr(p, '=', (size_t)(next - p));

        if (!equal) {
            return CALLBACK_INVALID;
        }

        char key[128];

        if (!query_decode(p, (size_t)(equal - p), key, sizeof(key))) {
            return CALLBACK_INVALID;
        }

        if (!strcmp(key, "state")) {
            if (has_state || !query_decode(equal + 1, (size_t)(next - equal - 1), received_state,
                                           sizeof(received_state))) {
                return CALLBACK_INVALID;
            }

            has_state = true;
        }

        if (!strcmp(key, "code")) {
            if (has_code || !query_decode(equal + 1, (size_t)(next - equal - 1), code, capacity)) {
                return CALLBACK_INVALID;
            }

            has_code = true;
        }

        if (!strcmp(key, "error")) {
            char error[128];

            if (denied || !query_decode(equal + 1, (size_t)(next - equal - 1), error, sizeof(error)) ||
                !error[0]) {
                return CALLBACK_INVALID;
            }

            denied = true;
        }

        p = next + (next < end);
    }

    if (!has_state || !*state || strcmp(state, received_state) || (denied && has_code)) {
        platform_wipe(code, capacity);
        return CALLBACK_INVALID;
    }

    if (denied) {
        return CALLBACK_DENIED;
    }
    return has_code && *code ? CALLBACK_CODE : CALLBACK_INVALID;
}

bool callback_parse(const char *request, const char *state, char *code, size_t capacity) {
    bool ok = callback_result(request, state, code, capacity) == CALLBACK_CODE;

    if (!ok) {
        platform_wipe(code, capacity);
    }
    return ok;
}

static bool token_string_valid(yyjson_val *value, size_t capacity) {
    if (!yyjson_is_str(value) || !yyjson_get_len(value) || yyjson_get_len(value) >= capacity) {
        return false;
    }

    const char *text = yyjson_get_str(value);

    return strlen(text) == yyjson_get_len(value) && !strchr(text, '\r') && !strchr(text, '\n');
}

bool tokens_parse(const char *json, uint64_t now, Tokens *tokens) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *access = yyjson_obj_get(root, "access_token");
    yyjson_val *refresh = yyjson_obj_get(root, "refresh_token");
    yyjson_val *expires = yyjson_obj_get(root, "expires_in");
    const char *type = yyjson_get_str(yyjson_obj_get(root, "token_type"));

    bool valid_expiry = yyjson_is_uint(expires) && yyjson_get_uint(expires) > 0 &&
                        yyjson_get_uint(expires) <= 86400 && now <= UINT64_MAX - yyjson_get_uint(expires);

    bool valid_refresh =
        refresh ? token_string_valid(refresh, sizeof(tokens->refresh)) : tokens->refresh[0] != 0;

    bool ok = type && !strcmp(type, "Bearer") && valid_expiry && valid_refresh &&
              token_string_valid(access, sizeof(tokens->access));

    if (ok) {
        snprintf(tokens->access, sizeof(tokens->access), "%s", yyjson_get_str(access));

        if (refresh) {
            snprintf(tokens->refresh, sizeof(tokens->refresh), "%s", yyjson_get_str(refresh));
        }

        tokens->expires_at = now + yyjson_get_uint(expires);
    }

    if (yyjson_is_str(access)) {
        platform_wipe((void *)yyjson_get_str(access), yyjson_get_len(access));
    }

    if (yyjson_is_str(refresh)) {
        platform_wipe((void *)yyjson_get_str(refresh), yyjson_get_len(refresh));
    }

    yyjson_doc_free(doc);

    return ok;
}

void copy_utf8(char *out, size_t capacity, const char *text) {
    if (!capacity) {
        return;
    }

    size_t length = strlen(text);
    size_t end = length < capacity ? length : capacity - 1;

    while (end > 0 && ((unsigned char)text[end] & 0xc0) == 0x80) {
        --end;
    }

    memcpy(out, text, end);
    out[end] = 0;
}

void playback_reset(Playback *playback) {
    memset(playback, 0, sizeof(*playback));
    playback->volume_percent = -1;
}

bool playback_parse(const char *json, Playback *playback) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    bool ok = yyjson_is_obj(root) && yyjson_is_bool(yyjson_obj_get(root, "is_playing"));

    if (ok) {
        playback_reset(playback);
        playback->playing = yyjson_get_bool(yyjson_obj_get(root, "is_playing"));
        playback->shuffle = yyjson_is_true(yyjson_obj_get(root, "shuffle_state"));
        const char *repeat = yyjson_get_str(yyjson_obj_get(root, "repeat_state"));
        playback->repeat = repeat && !strcmp(repeat, "track")     ? REPEAT_TRACK
                           : repeat && !strcmp(repeat, "context") ? REPEAT_CONTEXT
                                                                  : REPEAT_OFF;

        yyjson_val *item = yyjson_obj_get(root, "item");
        const char *uri = yyjson_get_str(yyjson_obj_get(item, "uri"));
        copy_utf8(playback->uri, sizeof(playback->uri), uri ? uri : "");
        const char *context_uri = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "context"), "uri"));
        copy_utf8(playback->context_uri, sizeof(playback->context_uri), context_uri ? context_uri : "");
        const char *type = yyjson_get_str(yyjson_obj_get(item, "type"));
        artwork_url_parse(
            yyjson_obj_get(type && !strcmp(type, "episode") ? item : yyjson_obj_get(item, "album"), "images"),
            playback->artwork);
        const char *title = yyjson_get_str(yyjson_obj_get(item, "name"));
        yyjson_val *first_artist = yyjson_arr_get(yyjson_obj_get(item, "artists"), 0);
        const char *artist = yyjson_get_str(yyjson_obj_get(first_artist, "name"));
        const char *artist_id = yyjson_get_str(yyjson_obj_get(first_artist, "id"));
        yyjson_val *album_obj = yyjson_obj_get(item, "album");
        const char *album = yyjson_get_str(yyjson_obj_get(album_obj, "name"));
        const char *album_id = yyjson_get_str(yyjson_obj_get(album_obj, "id"));

        if (!artist) {
            artist = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(item, "show"), "name"));
        }
        const char *device = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "device"), "name"));

        copy_utf8(playback->title, sizeof(playback->title), title ? title : "Nothing playing");
        copy_utf8(playback->artist, sizeof(playback->artist), artist ? artist : "");
        copy_utf8(playback->artist_id, sizeof(playback->artist_id),
                  artist_id && strlen(artist_id) == 22 ? artist_id : "");
        copy_utf8(playback->album, sizeof(playback->album), album ? album : "");
        copy_utf8(playback->album_id, sizeof(playback->album_id),
                  album_id && strlen(album_id) == 22 ? album_id : "");
        copy_utf8(playback->device, sizeof(playback->device), device ? device : "No active device");

        yyjson_val *source = yyjson_obj_get(root, "device");
        yyjson_val *id = yyjson_obj_get(source, "id");
        yyjson_val *volume = yyjson_obj_get(source, "volume_percent");
        yyjson_val *progress = yyjson_obj_get(root, "progress_ms");
        yyjson_val *duration = yyjson_obj_get(item, "duration_ms");

        if (yyjson_is_str(id) && yyjson_get_len(id) > 0 && yyjson_get_len(id) < sizeof(playback->device_id) &&
            strlen(yyjson_get_str(id)) == yyjson_get_len(id)) {
            snprintf(playback->device_id, sizeof(playback->device_id), "%s", yyjson_get_str(id));
            playback->has_device = yyjson_is_true(yyjson_obj_get(source, "is_active"));
        }

        playback->restricted = !yyjson_is_false(yyjson_obj_get(source, "is_restricted"));
        playback->supports_volume = yyjson_is_true(yyjson_obj_get(source, "supports_volume"));

        if (yyjson_is_uint(volume) && yyjson_get_uint(volume) <= 100) {
            playback->volume_percent = (int)yyjson_get_uint(volume);
        }

        if (yyjson_is_uint(duration) && yyjson_get_uint(duration) <= INT_MAX) {
            playback->duration_ms = (unsigned)yyjson_get_uint(duration);
        }

        if (yyjson_is_uint(progress) && yyjson_get_uint(progress) <= INT_MAX) {
            playback->position_ms = (unsigned)yyjson_get_uint(progress);
            playback->position_known = true;
        }

        playback->seeking_disallowed = yyjson_is_true(
            yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "actions"), "disallows"), "seeking"));
        yyjson_val *disallows = yyjson_obj_get(yyjson_obj_get(root, "actions"), "disallows");
        playback->pausing_disallowed = yyjson_is_true(yyjson_obj_get(disallows, "pausing"));
        playback->resuming_disallowed = yyjson_is_true(yyjson_obj_get(disallows, "resuming"));
        playback->skipping_next_disallowed = yyjson_is_true(yyjson_obj_get(disallows, "skipping_next"));
        playback->skipping_previous_disallowed = yyjson_is_true(yyjson_obj_get(disallows, "skipping_prev"));
    }

    yyjson_doc_free(doc);

    return ok;
}
