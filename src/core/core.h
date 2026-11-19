#ifndef NATIVE_CORE_H
#define NATIVE_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "artwork.h"

#define REDIRECT_URI "http://127.0.0.1:43821/callback"
#define DEVICE_ID_CAPACITY 256
#define AUTH_SCOPE                                                                                           \
    "user-read-playback-state user-read-currently-playing user-modify-playback-state user-library-read "     \
    "playlist-read-private playlist-read-collaborative user-read-recently-played "                           \
    "user-follow-read user-follow-modify user-library-modify user-read-private"

typedef struct {
    char verifier[44];
    char challenge[44];
    char state[44];
} Pkce;

typedef struct {
    char access[4096];
    char refresh[4096];
    uint64_t expires_at;
} Tokens;

typedef enum { REPEAT_OFF, REPEAT_CONTEXT, REPEAT_TRACK } RepeatMode;

typedef struct {
    bool playing;
    bool shuffle;
    RepeatMode repeat;
    char uri[128];
    char context_uri[128];
    char title[512];
    char artist[512];
    char artist_id[23];
    char album[512];
    char album_id[23];
    char device[256];
    char device_id[DEVICE_ID_CAPACITY];
    char artwork[ARTWORK_URL_CAPACITY];
    unsigned position_ms;
    unsigned duration_ms;
    int volume_percent;
    bool has_device;
    bool restricted;
    bool supports_volume;
    bool position_known;
    bool seeking_disallowed;
    bool pausing_disallowed;
    bool resuming_disallowed;
    bool skipping_next_disallowed;
    bool skipping_previous_disallowed;
    bool buffering;
    unsigned anchor_position_ms;
    uint64_t anchor_timestamp_ms;
} Playback;

bool base64url(const unsigned char *data, size_t size, char *out, size_t capacity);
bool url_encode(const char *text, char *out, size_t capacity);
bool pkce_create(Pkce *pkce);
bool client_id_valid(const char *client_id);
bool authorization_url(const char *client_id, const Pkce *pkce, char *out, size_t capacity);
bool callback_parse(const char *request, const char *state, char *code, size_t capacity);
typedef enum { CALLBACK_INVALID, CALLBACK_CODE, CALLBACK_DENIED } CallbackResult;
CallbackResult callback_result(const char *request, const char *state, char *code, size_t capacity);
bool tokens_parse(const char *json, uint64_t now, Tokens *tokens);
bool playback_parse(const char *json, Playback *playback);
void playback_reset(Playback *playback);
void copy_utf8(char *out, size_t capacity, const char *text);
#endif
