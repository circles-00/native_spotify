#ifndef NATIVE_PLAYER_H
#define NATIVE_PLAYER_H

#include "browse.h"
#include "core.h"

#define DEVICE_LIMIT 32
#define QUEUE_LIMIT 50

typedef struct {
    char id[DEVICE_ID_CAPACITY];
    char name[256];
    char type[32];
    bool active;
    bool restricted;
    bool supports_volume;
    int volume_percent;
} Device;

typedef struct {
    Device items[DEVICE_LIMIT];
    unsigned count;
    int selected;
    bool truncated;
} Devices;

typedef struct {
    CatalogItem current;
    CatalogItem items[QUEUE_LIMIT];
    unsigned count;
    unsigned received;
    unsigned queued;
    unsigned first;
    unsigned total;
    bool has_current;
    bool truncated;
} Queue;

typedef struct {
    unsigned revision;
    unsigned seek;
    unsigned volume;
} PlayerDraft;

void devices_reset(Devices *devices);
bool devices_parse(const char *json, Devices *devices);
bool devices_select(Devices *devices, int index);
const Device *devices_selected(const Devices *devices);
bool device_can_transfer(const Device *device);
bool device_transfer_body(const Device *device, bool playing, char *out, size_t capacity);

bool queue_parse(const char *json, Queue *queue);
const CatalogItem *queue_row(const Queue *queue, unsigned index);
bool queue_page(const Queue *queue, unsigned offset, BrowsePage *page);

bool playback_can_seek(const Playback *playback);
bool playback_can_volume(const Playback *playback);
bool playback_can_command(const Playback *playback, const char *command);
bool player_command_url(const Playback *playback, const char *command, char *out, size_t capacity);
unsigned player_position(const Playback *playback, uint64_t elapsed_ms);

typedef struct {
    uint64_t next_ms;
    unsigned settling;
    unsigned failures;
    unsigned engine_revision;
    uint64_t transport_until_ms;
    char transport_device[DEVICE_ID_CAPACITY];
    bool transport_playing;
    uint64_t track_until_ms;
    char track_device[DEVICE_ID_CAPACITY];
    char previous_uri[sizeof(((Playback *)0)->uri)];
    char expected_uri[sizeof(((Playback *)0)->uri)];
    unsigned restart_from_ms;
    char prefetched_uri[sizeof(((Playback *)0)->uri)];
} PlayerSync;

void player_sync_requested(PlayerSync *sync, uint64_t now_ms);
void player_sync_expect_playing(PlayerSync *sync, const char *device_id, bool playing, uint64_t now_ms);
bool player_sync_waiting(PlayerSync *sync, const Playback *incoming, uint64_t now_ms);
void player_sync_expect_track(PlayerSync *sync, const Playback *previous, const char *device,
                              const char *expected_uri, bool allow_restart, uint64_t now_ms);
void player_sync_received(PlayerSync *sync, uint64_t now_ms, bool success, bool playing,
                          uint64_t retry_at_ms);
bool player_seek_position(const Playback *playback, unsigned fraction, unsigned *position);
bool player_seek_url(const Playback *playback, unsigned position, char *out, size_t capacity);
bool player_volume_url(const Playback *playback, unsigned percent, char *out, size_t capacity);
bool player_options_url(const Playback *playback, bool shuffle, unsigned value, char *out, size_t capacity);
bool player_draft_sync(PlayerDraft *draft, const Playback *playback, unsigned revision);

typedef struct {
    bool pending;
    unsigned sequence;
    uint64_t deadline_ms;
    bool local;
    bool dispatched;
    unsigned target_ms;
    uint64_t previous_anchor_ms;
} PlayerSeek;

void player_seek_requested(PlayerSeek *seek, unsigned sequence, uint64_t now_ms);
void player_seek_completed(PlayerSeek *seek, unsigned sequence, uint64_t now_ms, bool accepted);
uint64_t player_local_seek_due(uint64_t last_dispatch_ms, bool queued, uint64_t now_ms);
bool player_seek_reconcile(PlayerSeek *seek, const Playback *expected, uint64_t received_ms,
                           Playback *incoming, uint64_t now_ms);
typedef enum { SEEK_WAITING, SEEK_READY, SEEK_TIMED_OUT } SeekResult;
SeekResult player_seek_reconcile_local(PlayerSeek *seek, const Playback *expected, const Playback *incoming,
                                       uint64_t now_ms);
void player_labels(const PlayerDraft *draft, const Playback *playback, char seek[96], char volume[64]);

#endif
