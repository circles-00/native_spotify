#include "player.h"
#include "catalog.h"

#include <stdio.h>
#include <string.h>

static bool device_id_valid(const char *id) {
    if (!id[0] || strlen(id) >= DEVICE_ID_CAPACITY) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) {
        if (*p < 32 || *p == 127) {
            return false;
        }
    }

    return true;
}

void devices_reset(Devices *devices) {
    memset(devices, 0, sizeof(*devices));
    devices->selected = -1;
}

bool devices_select(Devices *devices, int index) {
    if (index < -1 || (index >= 0 && (unsigned)index >= devices->count)) {
        return false;
    }

    devices->selected = index;

    return true;
}

const Device *devices_selected(const Devices *devices) {
    if (devices->selected < 0 || (unsigned)devices->selected >= devices->count) {
        return NULL;
    }

    return &devices->items[devices->selected];
}

bool device_can_transfer(const Device *device) {
    return device && !device->restricted && device_id_valid(device->id);
}

bool devices_parse(const char *json, Devices *devices) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(doc), "devices");
    bool valid = yyjson_is_arr(items);

    if (valid) {
        Devices parsed;
        devices_reset(&parsed);
        parsed.truncated = yyjson_arr_size(items) > DEVICE_LIMIT;

        const Device *previous = devices_selected(devices);
        int active = -1;

        for (size_t i = 0; i < yyjson_arr_size(items) && i < DEVICE_LIMIT; ++i) {
            yyjson_val *value = yyjson_arr_get(items, i);
            const char *name = yyjson_get_str(yyjson_obj_get(value, "name"));

            if (!yyjson_is_obj(value) || !name) {
                continue;
            }

            Device device = {.volume_percent = -1};
            yyjson_val *id = yyjson_obj_get(value, "id");
            yyjson_val *volume = yyjson_obj_get(value, "volume_percent");

            if (yyjson_is_str(id) && yyjson_get_len(id) < sizeof(device.id) &&
                strlen(yyjson_get_str(id)) == yyjson_get_len(id)) {
                snprintf(device.id, sizeof(device.id), "%s", yyjson_get_str(id));
            }

            copy_utf8(device.name, sizeof(device.name), name);
            const char *type = yyjson_get_str(yyjson_obj_get(value, "type"));
            copy_utf8(device.type, sizeof(device.type), type ? type : "");
            device.active = yyjson_is_true(yyjson_obj_get(value, "is_active"));
            device.restricted = !yyjson_is_false(yyjson_obj_get(value, "is_restricted"));
            device.supports_volume = yyjson_is_true(yyjson_obj_get(value, "supports_volume"));

            if (yyjson_is_uint(volume) && yyjson_get_uint(volume) <= 100) {
                device.volume_percent = (int)yyjson_get_uint(volume);
            }

            if (previous && device.id[0] && !strcmp(previous->id, device.id)) {
                parsed.selected = (int)parsed.count;
            }

            if (device.active) {
                active = (int)parsed.count;
            }

            parsed.items[parsed.count++] = device;
        }

        if (parsed.selected < 0 && parsed.count) {
            parsed.selected = active >= 0 ? active : 0;
        }

        *devices = parsed;
    }

    yyjson_doc_free(doc);

    return valid;
}

bool device_transfer_body(const Device *device, bool playing, char *out, size_t capacity) {
    if (!device_can_transfer(device)) {
        return false;
    }

    char escaped[sizeof(device->id) * 2];
    size_t length = 0;

    for (const char *p = device->id; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            escaped[length++] = '\\';
        }
        escaped[length++] = *p;
    }

    escaped[length] = 0;
    int size =
        snprintf(out, capacity, "{\"device_ids\":[\"%s\"],\"play\":%s}", escaped, playing ? "true" : "false");

    return size > 0 && (size_t)size < capacity;
}

static bool queue_item(yyjson_val *value, CatalogItem *item) {
    const char *type = yyjson_get_str(yyjson_obj_get(value, "type"));

    if (!type || (strcmp(type, "track") && strcmp(type, "episode"))) {
        return false;
    }

    return catalog_item_parse(value, !strcmp(type, "track") ? ITEM_TRACK : ITEM_EPISODE, item);
}

bool queue_parse(const char *json, Queue *queue) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);

    if (!doc) {
        return false;
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *items = yyjson_obj_get(root, "queue");
    bool valid = yyjson_is_arr(items) && yyjson_arr_size(items) <= UINT32_MAX;

    if (valid) {
        Queue parsed = {0};
        parsed.received = (unsigned)yyjson_arr_size(items);
        parsed.truncated = parsed.received > QUEUE_LIMIT;
        parsed.has_current = queue_item(yyjson_obj_get(root, "currently_playing"), &parsed.current);

        for (unsigned i = 0; i < parsed.received && i < QUEUE_LIMIT; ++i) {
            CatalogItem item;

            if (queue_item(yyjson_arr_get(items, i), &item)) {
                parsed.items[parsed.count++] = item;
            }
        }

        *queue = parsed;
    }

    yyjson_doc_free(doc);

    return valid;
}

/* Rows of a composed queue are a window starting at `first`; absolute indexes outside it return NULL. */
const CatalogItem *queue_row(const Queue *queue, unsigned index) {
    if (index < queue->first || index - queue->first >= queue->count) {
        return NULL;
    }

    return &queue->items[index - queue->first];
}

bool queue_page(const Queue *queue, unsigned offset, BrowsePage *page) {
    if ((queue->count && offset >= queue->count) || (!queue->count && offset) || offset % BROWSE_PAGE_SIZE) {
        return false;
    }

    BrowsePage parsed = {.offset = offset, .limit = BROWSE_PAGE_SIZE, .total = queue->count};

    for (unsigned i = offset; i < queue->count && parsed.count < BROWSE_PAGE_SIZE; ++i) {
        parsed.items[parsed.count++] = queue->items[i];
    }

    parsed.has_next = offset + parsed.count < queue->count;
    *page = parsed;

    return true;
}

bool playback_can_seek(const Playback *playback) {
    return playback->has_device && device_id_valid(playback->device_id) && !playback->restricted &&
           !playback->seeking_disallowed && playback->position_known && playback->duration_ms > 0;
}

bool playback_can_volume(const Playback *playback) {
    return playback->has_device && device_id_valid(playback->device_id) && !playback->restricted &&
           playback->supports_volume;
}

bool playback_can_command(const Playback *playback, const char *command) {
    if (!playback->has_device || !device_id_valid(playback->device_id) || playback->restricted) {
        return false;
    }

    if (!strcmp(command, "play")) {
        return !playback->resuming_disallowed;
    }
    if (!strcmp(command, "pause")) {
        return !playback->pausing_disallowed;
    }
    if (!strcmp(command, "next")) {
        return !playback->skipping_next_disallowed;
    }
    if (!strcmp(command, "previous")) {
        return !playback->skipping_previous_disallowed;
    }

    return false;
}

bool player_command_url(const Playback *playback, const char *command, char *out, size_t capacity) {
    if (!playback_can_command(playback, command)) {
        return false;
    }

    char id[DEVICE_ID_CAPACITY * 3];

    if (!url_encode(playback->device_id, id, sizeof(id))) {
        return false;
    }

    int size = snprintf(out, capacity, "https://api.spotify.com/v1/me/player/%s?device_id=%s", command, id);
    return size > 0 && (size_t)size < capacity;
}

bool player_options_url(const Playback *playback, bool shuffle, unsigned value, char *out, size_t capacity) {
    if (!playback->has_device || playback->restricted || !device_id_valid(playback->device_id) ||
        value > (shuffle ? 1u : (unsigned)REPEAT_TRACK)) {
        return false;
    }

    char id[DEVICE_ID_CAPACITY * 3];

    if (!url_encode(playback->device_id, id, sizeof(id))) {
        return false;
    }

    const char *state = shuffle                   ? (value ? "true" : "false")
                        : value == REPEAT_TRACK   ? "track"
                        : value == REPEAT_CONTEXT ? "context"
                                                  : "off";
    int size = snprintf(out, capacity, "https://api.spotify.com/v1/me/player/%s?state=%s&device_id=%s",
                        shuffle ? "shuffle" : "repeat", state, id);
    return size > 0 && (size_t)size < capacity;
}

unsigned player_position(const Playback *playback, uint64_t elapsed_ms) {
    if (!playback->playing || !playback->has_device || !playback->position_known || !playback->duration_ms) {
        return playback->position_ms;
    }

    uint64_t remaining =
        playback->duration_ms > playback->position_ms ? playback->duration_ms - playback->position_ms : 0;
    return elapsed_ms >= remaining ? playback->duration_ms : playback->position_ms + (unsigned)elapsed_ms;
}

void player_seek_requested(PlayerSeek *seek, unsigned sequence, uint64_t now_ms) {
    *seek = (PlayerSeek){true, sequence, now_ms + 30000};
}

void player_seek_completed(PlayerSeek *seek, unsigned sequence, uint64_t now_ms, bool accepted) {
    if (!seek->pending || seek->sequence != sequence) {
        return;
    }

    seek->pending = accepted;
    seek->deadline_ms = now_ms + 5000;
}

bool player_seek_reconcile(PlayerSeek *seek, const Playback *expected, uint64_t received_ms,
                           Playback *incoming, uint64_t now_ms) {
    if (!seek->pending) {
        return true;
    }

    bool same = incoming->has_device && !strcmp(expected->device_id, incoming->device_id) &&
                !strcmp(expected->uri, incoming->uri) && !strcmp(expected->title, incoming->title);
    unsigned position = player_position(expected, now_ms >= received_ms ? now_ms - received_ms : 0);
    unsigned difference = incoming->position_ms > position ? incoming->position_ms - position
                                                           : position - incoming->position_ms;

    if ((same && incoming->position_known && difference <= 1500) || now_ms >= seek->deadline_ms ||
        (incoming->has_device && !same)) {
        seek->pending = false;
        return true;
    }

    if (!same || !incoming->position_known) {
        return false;
    }

    incoming->position_ms = position;
    return true;
}

void player_sync_requested(PlayerSync *sync, uint64_t now_ms) {
    sync->settling = 3;
    sync->next_ms = now_ms + 400;
}

void player_sync_received(PlayerSync *sync, uint64_t now_ms, bool success, bool playing,
                          uint64_t retry_at_ms) {
    sync->failures = success ? 0 : sync->failures + (sync->failures < 6);
    uint64_t interval = success ? (playing ? 5000 : 15000) : 5000u << sync->failures;

    if (interval > 300000) {
        interval = 300000;
    }

    if (success && sync->settling) {
        --sync->settling;
        if (sync->settling) {
            interval = 1000;
        }
    }

    sync->next_ms = now_ms + interval;
    if (retry_at_ms > sync->next_ms) {
        sync->next_ms = retry_at_ms;
    }
}

bool player_seek_position(const Playback *playback, unsigned fraction, unsigned *position) {
    if (fraction > 1000 || !playback_can_seek(playback)) {
        return false;
    }

    unsigned value = (unsigned)((uint64_t)playback->duration_ms * fraction / 1000);
    *position = value < playback->duration_ms ? value : playback->duration_ms - 1;

    return true;
}

bool player_seek_url(const Playback *playback, unsigned position, char *out, size_t capacity) {
    if (!playback_can_seek(playback) || position >= playback->duration_ms) {
        return false;
    }

    char id[sizeof(playback->device_id) * 3];
    if (!url_encode(playback->device_id, id, sizeof(id))) {
        return false;
    }

    int size = snprintf(
        out, capacity, "https://api.spotify.com/v1/me/player/seek?position_ms=%u&device_id=%s", position, id);

    return size > 0 && (size_t)size < capacity;
}

bool player_volume_url(const Playback *playback, unsigned percent, char *out, size_t capacity) {
    if (percent > 100 || !playback_can_volume(playback)) {
        return false;
    }

    char id[sizeof(playback->device_id) * 3];
    if (!url_encode(playback->device_id, id, sizeof(id))) {
        return false;
    }

    int size =
        snprintf(out, capacity, "https://api.spotify.com/v1/me/player/volume?volume_percent=%u&device_id=%s",
                 percent, id);

    return size > 0 && (size_t)size < capacity;
}

bool player_draft_sync(PlayerDraft *draft, const Playback *playback, unsigned revision) {
    if (draft->revision == revision) {
        return false;
    }

    draft->revision = revision;
    draft->seek = playback->duration_ms
                      ? (unsigned)((uint64_t)playback->position_ms * 1000 / playback->duration_ms)
                      : 0;
    if (draft->seek > 1000) {
        draft->seek = 1000;
    }

    draft->volume = playback->volume_percent >= 0 ? (unsigned)playback->volume_percent : 0;

    return true;
}

void player_labels(const PlayerDraft *draft, const Playback *playback, char seek[96], char volume[64]) {
    unsigned position = 0;

    bool can_seek = player_seek_position(playback, draft->seek, &position);

    if (can_seek) {
        snprintf(seek, 96, "Seek %u:%02u / %u:%02u", position / 60000, position / 1000 % 60,
                 playback->duration_ms / 60000, playback->duration_ms / 1000 % 60);
    }

    if (!can_seek) {
        snprintf(seek, 96, "Seek unavailable");
    }

    if (!playback_can_volume(playback)) {
        snprintf(volume, 64, "Volume n/a");
        return;
    }

    if (playback->volume_percent < 0) {
        snprintf(volume, 64, "Volume ?");
        return;
    }

    snprintf(volume, 64, "Volume %u%%", draft->volume);
}
