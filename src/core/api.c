#include "api.h"
#include "catalog.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void response_free(HttpResponse *response) {
    if (response->body) {
        platform_wipe(response->body, response->size);
        free(response->body);
    }
}

static bool ensure_token(Spotify *spotify);
static bool request(Spotify *spotify, const char *method, const char *url, const char *body, bool form,
                    HttpResponse *response);

static bool request_url(Spotify *spotify, const char *url, HttpResponse *response) {
    return request(spotify, "GET", url, NULL, false, response);
}

static bool request(Spotify *spotify, const char *method, const char *url, const char *body, bool form,
                    HttpResponse *response) {
    uint64_t now = platform_seconds();

    if (now < spotify->retry_at) {
        spotify->last_status = 429;
        snprintf(spotify->message, sizeof(spotify->message), "Rate limited. Try again in %llu seconds.",
                 (unsigned long long)(spotify->retry_at - now));

        return false;
    }

    spotify->last_status = 0;

    if (!platform_http(method, url, form ? NULL : spotify->tokens.access, body, form, response)) {
        snprintf(spotify->message, sizeof(spotify->message), "Network/TLS error. Check your connection.");

        return false;
    }

    if (response->status == 401 && !form) {
        response_free(response);
        *response = (HttpResponse){0};
        spotify->tokens.expires_at = 0;

        if (!ensure_token(spotify)) {
            return false;
        }

        if (!platform_http(method, url, spotify->tokens.access, body, false, response)) {
            snprintf(spotify->message, sizeof(spotify->message),
                     "Network/TLS error after refreshing authorization.");
            return false;
        }
    }

    spotify->last_status = response->status;

    if (response->status >= 200 && response->status < 300) {
        return true;
    }

    switch (response->status) {
    case 429:
        spotify->retry_at = now + (response->retry_after ? response->retry_after : 60);
        snprintf(spotify->message, sizeof(spotify->message),
                 "Spotify quota/rate limit reached. Requests paused.");
        break;

    case 400:
        if (form) {
            platform_wipe(&spotify->tokens, sizeof(spotify->tokens));
            snprintf(spotify->message, sizeof(spotify->message),
                     "Spotify could not renew this session. Log in again in your browser.");
            break;
        }

        snprintf(spotify->message, sizeof(spotify->message), "Spotify rejected this playback request.");
        break;

    case 401:
        spotify->tokens.expires_at = 0;
        snprintf(spotify->message, sizeof(spotify->message),
                 "Authorization expired. Retry to refresh, or reconnect.");
        break;

    case 403:
        snprintf(spotify->message, sizeof(spotify->message),
                 "Access denied. Reconnect if scopes changed; check Premium and app allowlist.");
        break;

    case 404:
        snprintf(spotify->message, sizeof(spotify->message), "%s",
                 strstr(url, "/me/player")
                     ? "No active Spotify playback session. Choose a track to start playback."
                     : "Spotify could not find this item, or it is not available to your account.");
        break;

    default:
        snprintf(spotify->message, sizeof(spotify->message), "Spotify request failed (HTTP %d).",
                 response->status);
        break;
    }

    response_free(response);
    response->body = NULL;

    return false;
}

static bool exchange(Spotify *spotify, const char *body) {
    HttpResponse response = {0};

    if (!request(spotify, "POST", "https://accounts.spotify.com/api/token", body, true, &response)) {
        return false;
    }

    Tokens tokens = spotify->tokens;
    bool ok = tokens_parse(response.body, platform_seconds(), &tokens);
    response_free(&response);

    if (!ok) {
        snprintf(spotify->message, sizeof(spotify->message), "Spotify returned an invalid token response.");
    }

    if (ok && !platform_secret_save(spotify->client_id, tokens.refresh)) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Cannot save refresh token. Unlock your system credential store, then reconnect.");
        ok = false;
    }

    if (ok) {
        spotify->tokens = tokens;
    }

    platform_wipe(&tokens, sizeof(tokens));

    return ok;
}

static bool ensure_token(Spotify *spotify) {
    if (spotify->tokens.access[0] && spotify->tokens.expires_at > platform_seconds() + 30) {
        return true;
    }

    if (!spotify->tokens.refresh[0]) {
        snprintf(spotify->message, sizeof(spotify->message), "Connect your Spotify account first.");

        return false;
    }
    char encoded[12288];
    char body[12544];

    if (!url_encode(spotify->tokens.refresh, encoded, sizeof(encoded))) {
        return false;
    }

    snprintf(body, sizeof(body), "grant_type=refresh_token&client_id=%s&refresh_token=%s", spotify->client_id,
             encoded);

    bool ok = exchange(spotify, body);

    platform_wipe(encoded, sizeof(encoded));
    platform_wipe(body, sizeof(body));

    return ok;
}

bool spotify_login(Spotify *spotify) {
    if (!client_id_valid(spotify->client_id)) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This build has an invalid OAuth application configuration.");

        return false;
    }

    Pkce pkce = {0};
    char url[2048];
    char code[2048] = {0};
    char encoded[6144] = {0};
    char body[6656] = {0};
    char redirect[128];

    Listener listener = loopback_open();

    if (listener == INVALID_LISTENER) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Cannot bind 127.0.0.1:43821. Another login may be running.");

        return false;
    }

    bool ok = pkce_create(&pkce) && authorization_url(spotify->client_id, &pkce, url, sizeof(url));

    if (ok && !platform_open_url(url)) {
        loopback_close(listener);
        platform_wipe(&pkce, sizeof(pkce));
        snprintf(spotify->message, sizeof(spotify->message),
                 "Could not open your default browser. Configure a system browser and try Log in again.");
        return false;
    }

    if (ok) {
        if (spotify->notify) {
            spotify->notify(
                "Browser opened. Approve Spotify access, then return here. Waiting up to 120 seconds…");
        }
        ok = loopback_authorize(listener, pkce.state, code, sizeof(code));
    }

    loopback_close(listener);

    if (!ok) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Login denied, invalid callback, or timed out after 120 seconds.");
    }

    if (ok) {
        if (spotify->notify) {
            spotify->notify("Authorization received. Securing your session and loading Spotify…");
        }
        ok = url_encode(code, encoded, sizeof(encoded)) &&
             url_encode(REDIRECT_URI, redirect, sizeof(redirect));

        if (ok) {
            snprintf(body, sizeof(body),
                     "grant_type=authorization_code&client_id=%s&redirect_uri=%s&code=%s&code_verifier=%s",
                     spotify->client_id, redirect, encoded, pkce.verifier);

            platform_wipe(&spotify->tokens, sizeof(spotify->tokens));
            ok = exchange(spotify, body);
        }
    }

    platform_wipe(&pkce, sizeof(pkce));
    platform_wipe(code, sizeof(code));
    platform_wipe(encoded, sizeof(encoded));
    platform_wipe(body, sizeof(body));

    if (ok) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Connected. Refresh token saved in your system credential store.");
    }

    return ok;
}

bool spotify_restore(Spotify *spotify) {
    if (!client_id_valid(spotify->client_id)) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This build has an invalid OAuth application configuration.");

        return false;
    }

    if (!platform_secret_load(spotify->client_id, spotify->tokens.refresh, sizeof(spotify->tokens.refresh))) {
        snprintf(spotify->message, sizeof(spotify->message), "Connect to Spotify to get started.");

        return false;
    }

    return ensure_token(spotify);
}

bool spotify_ensure_token(Spotify *spotify) {
    return ensure_token(spotify);
}

bool spotify_disconnect(Spotify *spotify) {
    platform_wipe(&spotify->tokens, sizeof(spotify->tokens));

    if (!platform_secret_delete(spotify->client_id)) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Session cleared, but stored credential could not be deleted. Unlock credential store and "
                 "retry.");

        return false;
    }

    snprintf(spotify->message, sizeof(spotify->message),
             "Disconnected locally. Revoke app access in Spotify account settings if desired.");

    return true;
}

bool spotify_playback(Spotify *spotify, Playback *playback) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", "https://api.spotify.com/v1/me/player", NULL, false, &response)) {
        return false;
    }

    if (response.status == 204) {
        playback_reset(playback);
        snprintf(playback->title, sizeof(playback->title), "No active playback");
        snprintf(playback->device, sizeof(playback->device), "Open Spotify on a device to start playback");

        response_free(&response);
        snprintf(spotify->message, sizeof(spotify->message), "Connected; no active playback device.");

        return true;
    }

    bool ok = playback_parse(response.body, playback);
    response_free(&response);
    snprintf(spotify->message, sizeof(spotify->message), "%s",
             ok ? "Playback updated." : "Invalid playback response.");

    return ok;
}

bool spotify_command(Spotify *spotify, const Playback *target, const char *command) {
    bool post = !strcmp(command, "next") || !strcmp(command, "previous");

    if (!post && strcmp(command, "play") && strcmp(command, "pause")) {
        return false;
    }

    if (!ensure_token(spotify)) {
        return false;
    }

    char url[1024];

    if (!player_command_url(target, command, url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This action needs an active, unrestricted Spotify device. Choose one in Devices.");
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, post ? "POST" : "PUT", url, NULL, false, &response)) {
        return false;
    }

    response_free(&response);
    snprintf(spotify->message, sizeof(spotify->message),
             "Command accepted. Refresh to update playback state.");

    return true;
}

bool spotify_browse(Spotify *spotify, const BrowseRequest *browse, BrowsePage *page) {
    char url[1200];

    if (!browse_url(browse, url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message), "Invalid search or browse request.");
        return false;
    }

    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    bool ok = browse_parse(response.body, browse, page);
    response_free(&response);

    snprintf(spotify->message, sizeof(spotify->message), "%s",
             ok ? "Browse updated. Select a row to play a track or open a collection."
                : "Invalid browse response; previous results kept.");

    return ok;
}

bool spotify_recent_albums(Spotify *spotify, BrowsePage *page) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", "https://api.spotify.com/v1/me/player/recently-played?limit=50", NULL, false,
                 &response)) {
        return false;
    }

    bool ok = recent_albums_parse(response.body, page);
    response_free(&response);
    return ok;
}

bool spotify_collection(Spotify *spotify, ItemKind kind, const char *id, CollectionInfo *info) {
    if ((kind != ITEM_ALBUM && kind != ITEM_ARTIST) || !spotify_id_valid(id) || !ensure_token(spotify)) {
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/%s/%s", kind == ITEM_ALBUM ? "albums" : "artists",
             id);
    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    bool ok = collection_parse(response.body, kind, info) && !strcmp(info->item.id, id);
    response_free(&response);
    return ok;
}

bool spotify_popular(Spotify *spotify, const char *id, CollectionInfo *info) {
    if (!spotify_id_valid(id) || !ensure_token(spotify)) {
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/artists/%s/top-tracks", id);
    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    bool ok = collection_popular_parse(response.body, info);
    response_free(&response);
    return ok;
}

bool spotify_collection_saved(Spotify *spotify, const CatalogItem *item, bool *saved) {
    if ((item->kind != ITEM_ALBUM && item->kind != ITEM_ARTIST) || !spotify_id_valid(item->id) ||
        !ensure_token(spotify)) {
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/%s%s%s",
             item->kind == ITEM_ALBUM ? "albums/contains?ids=" : "following/contains?type=artist&ids=",
             item->id, "");
    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
    yyjson_val *value = yyjson_arr_get(yyjson_doc_get_root(doc), 0);
    bool ok = yyjson_is_bool(value);

    if (ok) {
        *saved = yyjson_get_bool(value);
    }

    yyjson_doc_free(doc);
    response_free(&response);
    return ok;
}

bool spotify_save_collection(Spotify *spotify, const CatalogItem *item, bool saved) {
    if ((item->kind != ITEM_ALBUM && item->kind != ITEM_ARTIST) || !spotify_id_valid(item->id) ||
        !ensure_token(spotify)) {
        return false;
    }

    char url[256];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/%s%s",
             item->kind == ITEM_ALBUM ? "albums?ids=" : "following?type=artist&ids=", item->id);
    HttpResponse response = {0};
    bool ok = request(spotify, saved ? "PUT" : "DELETE", url, NULL, false, &response);
    response_free(&response);
    return ok;
}

static bool song_library_request(Spotify *spotify, const char *method, const char *id,
                                 bool contains, HttpResponse *response) {
    if (!spotify_id_valid(id) || !ensure_token(spotify)) {
        return false;
    }

    char url[160];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/library%s?uris=spotify%%3Atrack%%3A%s",
             contains ? "/contains" : "", id);

    if (request(spotify, method, url, NULL, false, response)) {
        return true;
    }

    if (spotify->last_status != 404 && spotify->last_status != 405) {
        return false;
    }

    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/tracks%s?ids=%s",
             contains ? "/contains" : "", id);
    return request(spotify, method, url, NULL, false, response);
}

bool spotify_song_liked(Spotify *spotify, const char *id, bool *liked) {
    HttpResponse response = {0};

    if (!song_library_request(spotify, "GET", id, true, &response)) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
    yyjson_val *value = yyjson_arr_get(yyjson_doc_get_root(doc), 0);
    bool ok = yyjson_is_bool(value);
    *liked = yyjson_get_bool(value);
    yyjson_doc_free(doc);
    response_free(&response);

    if (!ok) {
        snprintf(spotify->message, sizeof(spotify->message), "Could not read this song's like status.");
    }

    return ok;
}

bool spotify_songs_liked(Spotify *spotify, const char ids[][23], unsigned count, bool *liked) {
    if (!count || count > LIKE_CHECK_LIMIT || !ensure_token(spotify)) {
        return false;
    }

    char url[64 + LIKE_CHECK_LIMIT * 48] = "https://api.spotify.com/v1/me/library/contains?uris=";

    for (unsigned i = 0; i < count; ++i) {
        if (!spotify_id_valid(ids[i])) {
            return false;
        }

        char entry[48];
        snprintf(entry, sizeof(entry), "%sspotify%%3Atrack%%3A%s", i ? "%2C" : "", ids[i]);
        strcat(url, entry);
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
    yyjson_val *values = yyjson_doc_get_root(doc);
    bool ok = yyjson_is_arr(values) && yyjson_arr_size(values) == count;

    for (unsigned i = 0; ok && i < count; ++i) {
        yyjson_val *value = yyjson_arr_get(values, i);
        ok = yyjson_is_bool(value);

        if (ok) {
            liked[i] = yyjson_get_bool(value);
        }
    }

    yyjson_doc_free(doc);
    response_free(&response);
    return ok;
}

bool spotify_tracks_page(Spotify *spotify, const BrowseRequest *request, TrackPage *page) {
    char url[256];

    if (!browse_tracks_url(request, url, sizeof(url)) || !ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request_url(spotify, url, &response)) {
        return false;
    }

    bool ok = browse_parse_tracks(response.body, request, page);
    response_free(&response);

    if (!ok) {
        snprintf(spotify->message, sizeof(spotify->message), "Invalid track list response; try again.");
    }

    return ok;
}

bool spotify_playlist_snapshot(Spotify *spotify, const char *id, char *out, size_t capacity) {
    char url[160];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/playlists/%s?fields=snapshot_id", id);

    if (!spotify_id_valid(id) || !ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request_url(spotify, url, &response)) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
    const char *snapshot = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "snapshot_id"));
    bool ok = snapshot && snapshot[0] && strlen(snapshot) < capacity;

    if (ok) {
        snprintf(out, capacity, "%s", snapshot);
    }

    yyjson_doc_free(doc);
    response_free(&response);
    return ok;
}

bool spotify_save_song(Spotify *spotify, const char *id, bool liked) {
    HttpResponse response = {0};
    bool ok = song_library_request(spotify, liked ? "PUT" : "DELETE", id, false, &response);
    response_free(&response);
    return ok;
}

bool spotify_song(Spotify *spotify, const char *id, CatalogItem *item) {
    if (!spotify_id_valid(id) || !ensure_token(spotify)) {
        return false;
    }

    char url[128];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/tracks/%s", id);
    HttpResponse response = {0};

    if (!request(spotify, "GET", url, NULL, false, &response)) {
        return false;
    }

    yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
    bool ok = catalog_item_parse(yyjson_doc_get_root(doc), ITEM_TRACK, item);
    yyjson_doc_free(doc);
    response_free(&response);
    return ok;
}

bool spotify_profile(Spotify *spotify, ProfileInfo *profile) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", "https://api.spotify.com/v1/me", NULL, false, &response)) {
        return false;
    }

    bool ok = profile_parse(response.body, profile);
    response_free(&response);
    return ok;
}

bool spotify_enqueue(Spotify *spotify, const Playback *target, const CatalogItem *item) {
    if (!item_can_play(item) || !ensure_token(spotify)) {
        return false;
    }

    char url[1024];
    const char *kind = item->kind == ITEM_EPISODE ? "episode" : "track";

    if (target && target->has_device && target->device_id[0]) {
        char encoded_device[DEVICE_ID_CAPACITY * 3];

        if (!url_encode(target->device_id, encoded_device, sizeof(encoded_device))) {
            return false;
        }

        snprintf(url, sizeof(url),
                 "https://api.spotify.com/v1/me/player/queue?uri=spotify%%3A%s%%3A%s&device_id=%s", kind,
                 item->id, encoded_device);
    } else {
        snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/player/queue?uri=spotify%%3A%s%%3A%s", kind,
                 item->id);
    }

    HttpResponse response = {0};
    bool ok = request(spotify, "POST", url, NULL, false, &response);
    response_free(&response);

    if (ok) {
        snprintf(spotify->message, sizeof(spotify->message), "Added to queue.");
    }

    return ok;
}

static bool radio_track_is_new(const CatalogItem *track, const CatalogItem *seed, const CatalogItem *tracks,
                              unsigned count) {
    if (track->kind != ITEM_TRACK || !item_can_play(track) || !strcmp(track->id, seed->id)) {
        return false;
    }

    for (unsigned i = 0; i < count; ++i) {
        if (!strcmp(track->id, tracks[i].id)) {
            return false;
        }
    }

    return true;
}

static unsigned radio_recommendations(Spotify *spotify, const CatalogItem *seed, CatalogItem *tracks,
                                      unsigned limit) {
    if (!ensure_token(spotify)) {
        return 0;
    }

    unsigned count = 0;

    if (spotify_id_valid(seed->id)) {
        char rec_url[512];
        snprintf(rec_url, sizeof(rec_url), "https://api.spotify.com/v1/recommendations?%s=%s&limit=%u",
                 seed->kind == ITEM_ARTIST ? "seed_artists" : "seed_tracks", seed->id, limit);
        HttpResponse response = {0};

        if (request(spotify, "GET", rec_url, NULL, false, &response)) {
            yyjson_doc *doc = yyjson_read(response.body, strlen(response.body), 0);
            yyjson_val *arr = yyjson_obj_get(yyjson_doc_get_root(doc), "tracks");

            for (size_t i = 0; i < yyjson_arr_size(arr) && count < limit; ++i) {
                if (catalog_item_parse(yyjson_arr_get(arr, i), ITEM_TRACK, &tracks[count]) &&
                    radio_track_is_new(&tracks[count], seed, tracks, count)) {
                    ++count;
                }
            }

            yyjson_doc_free(doc);
        }

        response_free(&response);
    }

    return count;
}

/* Tops up the list with the seed artist's songs when recommendations come back short. */
static unsigned radio_fill_from_search(Spotify *spotify, const CatalogItem *seed, CatalogItem *tracks,
                                       unsigned count, unsigned limit) {
    BrowseRequest search = {.kind = BROWSE_SEARCH, .filter = SEARCH_SONGS};
    const char *query = seed->detail[0] ? seed->detail : seed->title;

    if (!query[0] || !search_append(search.query, query, strlen(query))) {
        return count;
    }

    for (unsigned attempt = 0; attempt < 12 && count < limit; ++attempt) {
        BrowsePage page;

        if (!spotify_browse(spotify, &search, &page)) {
            break;
        }

        for (unsigned i = 0; i < page.count && count < limit; ++i) {
            if (radio_track_is_new(&page.items[i], seed, tracks, count)) {
                tracks[count++] = page.items[i];
            }
        }

        if (!page.has_next || !page.limit) {
            break;
        }

        search.offset = page.offset + page.limit;
    }

    return count;
}

unsigned spotify_radio_tracks(Spotify *spotify, const CatalogItem *seed, CatalogItem *tracks, unsigned limit) {
    unsigned found = radio_recommendations(spotify, seed, tracks, limit);
    return radio_fill_from_search(spotify, seed, tracks, found, limit);
}

bool spotify_song_radio(Spotify *spotify, const CatalogItem *seed, SongRadio *radio) {
    if (seed->kind != ITEM_TRACK || !spotify_id_valid(seed->id)) {
        snprintf(spotify->message, sizeof(spotify->message), "Song radio requires a Spotify track.");
        return false;
    }

    radio->tracks[0] = *seed;
    radio->count = 1 + spotify_radio_tracks(spotify, seed, radio->tracks + 1, SONG_RADIO_LIMIT - 1);

    if (radio->count == 1 && spotify->last_status >= 200 && spotify->last_status < 300) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Spotify returned no recommendations or songs for this artist.");
    }

    if (radio->count == 1 && (spotify->last_status < 200 || spotify->last_status >= 300)) {
        char reason[160];
        copy_utf8(reason, sizeof(reason), spotify->message);
        snprintf(spotify->message, sizeof(spotify->message), "Could not load radio songs (HTTP %d): %s",
                 spotify->last_status, reason);
    }

    return true;
}

bool spotify_play_tracks(Spotify *spotify, const Playback *target, const char ids[][23], unsigned count) {
    char body[sizeof("{\"uris\":[]}") + SONG_RADIO_LIMIT * (sizeof("\"spotify:track:") + sizeof(ids[0]))] =
        "{\"uris\":[";

    if (!count || count > SONG_RADIO_LIMIT || !ensure_token(spotify)) {
        return false;
    }

    for (unsigned i = 0; i < count; ++i) {
        if (!spotify_id_valid(ids[i])) {
            return false;
        }

        char entry[64];
        snprintf(entry, sizeof(entry), "%s\"spotify:track:%s\"", i ? "," : "", ids[i]);
        strcat(body, entry);
    }

    strcat(body, "]}");
    char url[1024];

    if (!player_command_url(target, "play", url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Choose a Spotify device before starting radio.");
        return false;
    }

    HttpResponse response = {0};
    bool ok = request(spotify, "PUT", url, body, false, &response);
    response_free(&response);
    return ok;
}

static bool valid_track_or_episode_uri(const char *uri, ItemKind *kind_out) {
    if (!strncmp(uri, "spotify:track:", 14) && spotify_id_valid(uri + 14)) {
        if (kind_out) {
            *kind_out = ITEM_TRACK;
        }
        return true;
    }

    if (!strncmp(uri, "spotify:episode:", 16) && spotify_id_valid(uri + 16)) {
        if (kind_out) {
            *kind_out = ITEM_EPISODE;
        }
        return true;
    }

    return false;
}

static bool valid_offset_context_uri(const char *uri) {
    if (!strncmp(uri, "spotify:playlist:", 17) && spotify_id_valid(uri + 17)) {
        return true;
    }

    if (!strncmp(uri, "spotify:album:", 14) && spotify_id_valid(uri + 14)) {
        return true;
    }

    return false;
}

bool spotify_play_restored(Spotify *spotify, const Playback *target, const Playback *restored,
                           const Queue *queue) {
    if (!restored || !valid_track_or_episode_uri(restored->uri, NULL) || !ensure_token(spotify)) {
        return false;
    }

    char url[1024];

    if (!player_command_url(target, "play", url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This action needs an active, unrestricted Spotify device. Choose one in Devices.");
        return false;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    if (!root) {
        yyjson_mut_doc_free(doc);
        return false;
    }

    if (valid_offset_context_uri(restored->context_uri)) {
        yyjson_mut_val *offset = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, root, "context_uri", restored->context_uri);
        if (offset) {
            yyjson_mut_obj_add_str(doc, offset, "uri", restored->uri);
            yyjson_mut_obj_add_val(doc, root, "offset", offset);
        }
    } else {
        yyjson_mut_val *uris = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_str(doc, uris, restored->uri);

        if (queue) {
            for (unsigned i = 0; i < queue->count && i < 20; ++i) {
                const CatalogItem *item = &queue->items[i];

                if (!item_can_play(item)) {
                    continue;
                }

                char quri[64];
                snprintf(quri, sizeof(quri), "spotify:%s:%s",
                         item->kind == ITEM_EPISODE ? "episode" : "track", item->id);
                yyjson_mut_arr_add_strcpy(doc, uris, quri);
            }
        }

        yyjson_mut_obj_add_val(doc, root, "uris", uris);
    }

    if (restored->position_ms > 0) {
        yyjson_mut_obj_add_uint(doc, root, "position_ms", restored->position_ms);
    }

    char *body = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);

    if (!body) {
        return false;
    }

    HttpResponse response = {0};
    bool ok = request(spotify, "PUT", url, body, false, &response);
    free(body);
    response_free(&response);
    return ok;
}

bool spotify_play_radio(Spotify *spotify, const Playback *target, const CatalogItem *seed, bool play_first) {
    CatalogItem tracks[BROWSE_PAGE_SIZE];
    unsigned count = radio_recommendations(spotify, seed, tracks, BROWSE_PAGE_SIZE);

    if (!count) {
        count = radio_fill_from_search(spotify, seed, tracks, count, BROWSE_PAGE_SIZE);
    }

    if (!count) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Could not find radio tracks for this selection.");
        return false;
    }

    if (play_first) {
        char ids[BROWSE_PAGE_SIZE][23];

        for (unsigned i = 0; i < count; ++i) {
            memcpy(ids[i], tracks[i].id, sizeof(ids[i]));
        }

        return spotify_play_tracks(spotify, target, (const char (*)[23])ids, count);
    }

    unsigned queued = 0;

    for (unsigned i = 0; i < count && queued < 5; ++i) {
        if (spotify_enqueue(spotify, target, &tracks[i])) {
            ++queued;
        }
    }

    return queued > 0;
}

bool spotify_play_item(Spotify *spotify, const Playback *target, const CatalogItem *item,
                       const BrowseRequest *context) {
    char body[256];

    if (!item_context_body(item, context, body, sizeof(body))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This row is not a playable Spotify track or episode.");
        return false;
    }

    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};
    char url[1024];

    if (!player_command_url(target, "play", url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Choose a Spotify device before playing a track.");
        return false;
    }

    if (!request(spotify, "PUT", url, body, false, &response)) {
        return false;
    }

    response_free(&response);
    snprintf(spotify->message, sizeof(spotify->message),
             "Track sent to your Spotify device. Refresh to update playback.");

    return true;
}

bool spotify_devices(Spotify *spotify, Devices *devices) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", "https://api.spotify.com/v1/me/player/devices", NULL, false, &response)) {
        return false;
    }

    bool ok = devices_parse(response.body, devices);
    response_free(&response);

    snprintf(spotify->message, sizeof(spotify->message), "%s",
             !ok ? "Invalid device response; previous list kept."
             : devices->truncated
                 ? "Device snapshot updated (first 32 entries only)."
                 : "Device snapshot updated. Select a device and Transfer; Refresh confirms the result.");

    return ok;
}

bool spotify_queue(Spotify *spotify, Queue *queue) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "GET", "https://api.spotify.com/v1/me/player/queue", NULL, false, &response)) {
        return false;
    }

    bool ok = queue_parse(response.body, queue);
    response_free(&response);

    if (ok) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Queue snapshot: %u supported entries from %u returned%s. Pages are local; Queue reloads.",
                 queue->count, queue->received, queue->truncated ? " (first 50 inspected)" : "");
    }

    if (!ok) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Invalid queue response; previous snapshot kept.");
    }

    return ok;
}

static bool player_put(Spotify *spotify, const char *url, const char *body) {
    if (!ensure_token(spotify)) {
        return false;
    }

    HttpResponse response = {0};

    if (!request(spotify, "PUT", url, body, false, &response)) {
        return false;
    }

    response_free(&response);
    snprintf(spotify->message, sizeof(spotify->message),
             "Command accepted. Refresh confirms playback/device state.");

    return true;
}

bool spotify_options(Spotify *spotify, const Playback *target, bool shuffle, unsigned value) {
    char url[1024];

    if (!player_options_url(target, shuffle, value, url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Playback options are unavailable on this device.");
        return false;
    }

    return player_put(spotify, url, NULL);
}

bool spotify_transfer(Spotify *spotify, const Device *device, bool playing) {
    char body[640];

    if (!device_transfer_body(device, playing, body, sizeof(body))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "This device has no usable ID or is restricted.");
        return false;
    }

    return player_put(spotify, "https://api.spotify.com/v1/me/player", body);
}

bool spotify_seek(Spotify *spotify, const Playback *playback, unsigned position) {
    char url[1024];

    if (!player_seek_url(playback, position, url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Seeking is unavailable or the position is outside this item.");
        return false;
    }

    return player_put(spotify, url, NULL);
}

bool spotify_volume(Spotify *spotify, const Playback *playback, unsigned percent) {
    char url[1024];

    if (!player_volume_url(playback, percent, url, sizeof(url))) {
        snprintf(spotify->message, sizeof(spotify->message),
                 "Volume control is unavailable or outside 0-100%%.");
        return false;
    }

    return player_put(spotify, url, NULL);
}
