#include "api.h"
#include "player.h"
#include "player_fixture.h"
#include "soloist.h"
#include "yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

#define CHECK(expression)                                                                                    \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(expression)) {                                                                                 \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression);                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void test_devices(void) {
    Devices devices;
    devices_reset(&devices);
    CHECK(!devices_selected(&devices));
    CHECK(!devices_select(&devices, 0));

    CHECK(devices_parse(DEVICES_FIXTURE, &devices));
    CHECK(devices.count == 3 && devices.selected == 2);
    CHECK(!strcmp(devices_selected(&devices)->name, "Desktop"));
    CHECK(devices_selected(&devices)->volume_percent == 65);
    CHECK(device_can_transfer(devices_selected(&devices)));

    char body[640];
    CHECK(device_transfer_body(devices_selected(&devices), false, body, sizeof(body)));

    yyjson_doc *doc = yyjson_read(body, strlen(body), 0);
    CHECK(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *ids = yyjson_obj_get(root, "device_ids");
    CHECK(yyjson_arr_size(ids) == 1);
    CHECK(!strcmp(yyjson_get_str(yyjson_arr_get(ids, 0)), "speaker\"\\&id"));
    CHECK(yyjson_is_false(yyjson_obj_get(root, "play")));
    yyjson_doc_free(doc);

    CHECK(!device_transfer_body(devices_selected(&devices), false, body, 8));
    CHECK(devices_select(&devices, 1));
    CHECK(!device_can_transfer(devices_selected(&devices)));
    CHECK(!device_transfer_body(devices_selected(&devices), false, body, sizeof(body)));
    CHECK(!devices_select(&devices, 3));
    CHECK(devices.selected == 1);

    CHECK(devices_select(&devices, 0));
    CHECK(devices_parse("{\"devices\":[{\"id\":\"new\",\"name\":\"New\",\"is_active\":true},"
                        "{\"id\":\"other\",\"name\":\"Phone\",\"is_restricted\":false}]}",
                        &devices));
    CHECK(devices.selected == 1);

    Devices before = devices;
    CHECK(!devices_parse("{\"devices\":null}", &devices));
    CHECK(!memcmp(&devices, &before, sizeof(devices)));
    CHECK(!devices_parse("bad", &devices));

    CHECK(devices_parse("{\"devices\":[]}", &devices));
    CHECK(!devices.count && devices.selected == -1);

    char many[8192] = "{\"devices\":[";

    for (unsigned i = 0; i < DEVICE_LIMIT + 1; ++i) {
        char item[128];
        snprintf(item, sizeof(item), "%s{\"id\":\"%u\",\"name\":\"Device\",\"is_restricted\":false}",
                 i ? "," : "", i);
        strcat(many, item);
    }

    strcat(many, "]}");
    CHECK(devices_parse(many, &devices));
    CHECK(devices.count == DEVICE_LIMIT && devices.truncated);

    Device longest = {0};
    memset(longest.id, '"', sizeof(longest.id) - 1);
    CHECK(device_transfer_body(&longest, false, body, sizeof(body)));

    longest.id[0] = '\n';
    CHECK(!device_can_transfer(&longest));
}

static void test_queue(void) {
    Queue queue = {0};
    CHECK(queue_parse("{\"currently_playing\":" QUEUE_EPISODE ",\"queue\":[" QUEUE_TRACK
                      ",null," QUEUE_EPISODE ",{\"type\":\"unknown\"}]}",
                      &queue));
    CHECK(queue.has_current && queue.current.kind == ITEM_EPISODE);
    CHECK(!strcmp(queue.current.detail, "Show"));
    CHECK(queue.count == 2 && queue.received == 4 && !queue.truncated);
    CHECK(queue.items[0].kind == ITEM_TRACK && queue.items[1].kind == ITEM_EPISODE);
    CHECK(!item_can_play(&queue.items[1]));

    CatalogItem episode = queue.items[1];
    episode.playable = true;
    char body[128];
    CHECK(item_play_body(&episode, body, sizeof(body)));
    CHECK(!strcmp(body, "{\"uris\":[\"spotify:episode:abcdefghijkl0123456789\"]}"));
    CHECK(item_link(&episode, body, sizeof(body)));
    CHECK(!strcmp(body, "https://open.spotify.com/episode/abcdefghijkl0123456789"));

    Queue before = queue;
    CHECK(!queue_parse("{\"queue\":null}", &queue));
    CHECK(!memcmp(&queue, &before, sizeof(queue)));
    CHECK(!queue_parse("bad", &queue));
    CHECK(queue_parse("{\"currently_playing\":null,\"queue\":[]}", &queue));
    CHECK(!queue.has_current && !queue.count);

    BrowsePage page;
    CHECK(queue_page(&queue, 0, &page));
    CHECK(!page.count && !page.has_next);
    CHECK(!queue_page(&queue, 10, &page));

    char many[8192] = "{\"queue\":[";

    for (unsigned i = 0; i < QUEUE_LIMIT + 1; ++i) {
        if (i) {
            strcat(many, ",");
        }
        strcat(many, QUEUE_TRACK);
    }

    strcat(many, "]}");
    CHECK(queue_parse(many, &queue));
    CHECK(queue.count == QUEUE_LIMIT && queue.received == QUEUE_LIMIT + 1 && queue.truncated);

    CHECK(queue_page(&queue, 0, &page));
    CHECK(page.count == 10 && page.has_next && page.total == 50);
    CHECK(queue_page(&queue, 40, &page));
    CHECK(page.count == 10 && !page.has_next);
    CHECK(!queue_page(&queue, 50, &page));
    CHECK(!queue_page(&queue, 1, &page));

    Browser browser;
    browser_reset(&browser);
    BrowseRequest request = {.kind = BROWSE_QUEUE, .offset = 40};
    browser_commit(&browser, &request, &page);
    CHECK(browser_page_request(&browser, -1, &request) && request.offset == 30);
    CHECK(!browser_page_request(&browser, 1, &request));

    CHECK(browser_select(&browser, 0));
    browser.page.items[0] = episode;
    CHECK(!browser_child_request(&browser, &request));
}

static void test_playback_controls(void) {
    Playback playback;
    CHECK(playback_parse(PLAYER_FIXTURE, &playback));
    CHECK(playback.has_device && !playback.restricted && playback.supports_volume);
    CHECK(playback.position_known && playback.position_ms == 60000 && playback.duration_ms == 240000);
    CHECK(playback.volume_percent == 65);
    CHECK(playback_can_seek(&playback) && playback_can_volume(&playback));

    unsigned position;
    CHECK(player_seek_position(&playback, 0, &position) && position == 0);
    CHECK(player_seek_position(&playback, 250, &position) && position == 60000);
    CHECK(player_seek_position(&playback, 1000, &position) && position == 239999);
    CHECK(!player_seek_position(&playback, 1001, &position));

    char url[1024];
    CHECK(player_seek_url(&playback, 0, url, sizeof(url)));
    CHECK(
        !strcmp(url, "https://api.spotify.com/v1/me/player/seek?position_ms=0&device_id=speaker%22%5C%26id"));
    CHECK(!player_seek_url(&playback, 240000, url, sizeof(url)));
    CHECK(!player_seek_url(&playback, 0, url, 8));

    CHECK(player_volume_url(&playback, 0, url, sizeof(url)));
    CHECK(strstr(url, "volume_percent=0&device_id=speaker%22%5C%26id") != NULL);
    CHECK(player_volume_url(&playback, 100, url, sizeof(url)));
    CHECK(!player_volume_url(&playback, 101, url, sizeof(url)));

    PlayerDraft draft = {0};
    CHECK(player_draft_sync(&draft, &playback, 1));
    CHECK(draft.seek == 250 && draft.volume == 65);
    draft.seek = 500;
    CHECK(!player_draft_sync(&draft, &playback, 1) && draft.seek == 500);
    CHECK(player_draft_sync(&draft, &playback, 2) && draft.seek == 250);

    char seek[96];
    char volume[64];
    player_labels(&draft, &playback, seek, volume);
    CHECK(!strcmp(seek, "Seek 1:00 / 4:00"));
    CHECK(!strcmp(volume, "Volume 65%"));

    playback.seeking_disallowed = true;
    CHECK(!playback_can_seek(&playback) && playback_can_volume(&playback));
    playback.seeking_disallowed = false;
    playback.supports_volume = false;
    CHECK(playback_can_seek(&playback) && !playback_can_volume(&playback));
    playback.restricted = true;
    CHECK(!playback_can_seek(&playback) && !playback_can_volume(&playback));

    CHECK(playback_parse("{\"is_playing\":false,\"item\":null,\"progress_ms\":null}", &playback));
    CHECK(!playback.position_known && playback.volume_percent == -1);
    CHECK(!playback_can_seek(&playback) && !playback_can_volume(&playback));
    player_labels(&draft, &playback, seek, volume);
    CHECK(!strcmp(seek, "Seek unavailable") && !strcmp(volume, "Volume n/a"));

    CHECK(playback_parse("{\"is_playing\":true,\"item\":{\"duration_ms\":1},\"progress_ms\":2147483648}",
                         &playback));
    CHECK(!playback.position_known && !playback_can_seek(&playback));

    CHECK(playback_parse("{\"is_playing\":true,\"device\":{\"id\":\"id\\u0000suffix\",\"is_active\":true}}",
                         &playback));
    CHECK(!playback.has_device);

    playback_reset(&playback);
    CHECK(!playback.has_device && !playback.duration_ms && playback.volume_percent == -1);
}

static void test_no_session_requests(void) {
    Spotify spotify = {0};
    Playback playback;
    CHECK(playback_parse(PLAYER_FIXTURE, &playback));

    Devices devices;
    devices_reset(&devices);
    CHECK(devices_parse(DEVICES_FIXTURE, &devices));
    CHECK(!spotify_transfer(&spotify, devices_selected(&devices), false));
    CHECK(!strcmp(spotify.message, "Connect your Spotify account first."));
    CHECK(!spotify_seek(&spotify, &playback, 1000));
    CHECK(!strcmp(spotify.message, "Connect your Spotify account first."));
    CHECK(!spotify_volume(&spotify, &playback, 50));
    CHECK(!strcmp(spotify.message, "Connect your Spotify account first."));
    CHECK(!spotify_seek(&spotify, &playback, playback.duration_ms));
    CHECK(strstr(spotify.message, "outside this item") != NULL);
}

static void test_commands_and_sync(void) {
    Playback playback;
    CHECK(playback_parse(PLAYER_FIXTURE, &playback));
    char url[1024];
    CHECK(player_command_url(&playback, "play", url, sizeof(url)));
    CHECK(strstr(url, "/play?device_id=") != NULL);
    CHECK(!strcmp(url, "https://api.spotify.com/v1/me/player/play?device_id=speaker%22%5C%26id"));
    CHECK(!player_command_url(&playback, "next/../../tokens", url, sizeof(url)));
    CHECK(!player_command_url(&playback, "play", url, 8));

    playback.pausing_disallowed = true;
    playback.resuming_disallowed = true;
    playback.skipping_next_disallowed = true;
    playback.skipping_previous_disallowed = true;
    CHECK(!playback_can_command(&playback, "pause"));
    CHECK(!playback_can_command(&playback, "play"));
    CHECK(!playback_can_command(&playback, "next"));
    CHECK(!playback_can_command(&playback, "previous"));

    CHECK(playback_parse(PLAYER_FIXTURE, &playback));
    CHECK(player_position(&playback, 1000) == playback.position_ms + 1000);
    CHECK(player_position(&playback, UINT64_MAX) == playback.duration_ms);
    playback.playing = false;
    CHECK(player_position(&playback, 1000) == playback.position_ms);
    playback.playing = true;
    playback.position_known = false;
    CHECK(player_position(&playback, 1000) == playback.position_ms);
    playback.position_known = true;
    playback.has_device = false;
    CHECK(player_position(&playback, 1000) == playback.position_ms);
    CHECK(!player_command_url(&playback, "play", url, sizeof(url)));

    Device device = {.id = "test-device", .restricted = false};
    char body[640];
    CHECK(device_transfer_body(&device, true, body, sizeof(body)));
    CHECK(strstr(body, "\"play\":true") != NULL);

    PlayerSync sync = {0};
    player_sync_requested(&sync, 1000);
    CHECK(sync.next_ms == 1000 && sync.settling == 3);
    player_sync_received(&sync, 1400, true, true, 0);
    CHECK(sync.next_ms == 2400 && sync.settling == 2);
    player_sync_received(&sync, 2400, true, true, 0);
    CHECK(sync.next_ms == 3400 && sync.settling == 1);
    player_sync_received(&sync, 3400, true, true, 0);
    CHECK(sync.next_ms == 8400 && sync.settling == 0);
    player_sync_received(&sync, 8400, true, false, 0);
    CHECK(sync.next_ms == 23400);
    player_sync_received(&sync, 23400, false, false, 0);
    CHECK(sync.failures == 1 && sync.next_ms == 33400);
    player_sync_received(&sync, 33400, false, false, 90000);
    CHECK(sync.next_ms == 90000 && sync.failures == 2);

    for (unsigned i = 0; i < 10; ++i) {
        player_sync_received(&sync, 90000, false, false, 0);
    }

    CHECK(sync.next_ms == 390000 && sync.failures == 6);
    player_sync_received(&sync, 390000, true, true, 0);
    CHECK(sync.next_ms == 395000 && sync.failures == 0);

    Playback ending = {.playing = true, .position_known = true, .position_ms = 178000, .duration_ms = 180000};
    player_sync_near_track_end(&sync, &ending, 390000, 394000);
    CHECK(sync.next_ms == 395000);
    player_sync_near_track_end(&sync, &ending, 390000, 0);
    CHECK(sync.next_ms == 392250);
    ending.position_ms = ending.duration_ms;
    player_sync_near_track_end(&sync, &ending, 390000, 0);
    CHECK(sync.next_ms == 392250);

    Playback incoming = {.has_device = true, .device_id = "test-device", .playing = true};
    player_sync_expect_playing(&sync, "test-device", false, 400000);
    CHECK(player_sync_waiting(&sync, &incoming, 400100));
    CHECK(!player_sync_waiting(&sync, &incoming, 401000));

    player_sync_expect_playing(&sync, "test-device", false, 402000);
    incoming.playing = false;
    CHECK(!player_sync_waiting(&sync, &incoming, 402100));
    incoming.playing = true;
    CHECK(!player_sync_waiting(&sync, &incoming, 402200));

    player_sync_expect_playing(&sync, "other-device", false, 403000);
    CHECK(!player_sync_waiting(&sync, &incoming, 403100));

    copy_utf8(incoming.uri, sizeof(incoming.uri), "spotify:track:old");
    player_sync_expect_track(&sync, &incoming, "test-device", "spotify:track:new", false, 404000);
    CHECK(player_sync_waiting(&sync, &incoming, 404100));
    copy_utf8(incoming.uri, sizeof(incoming.uri), "spotify:track:new");
    CHECK(!player_sync_waiting(&sync, &incoming, 404200));
    copy_utf8(incoming.uri, sizeof(incoming.uri), "spotify:track:old");
    CHECK(player_sync_waiting(&sync, &incoming, 404300));
    CHECK(!player_sync_waiting(&sync, &incoming, 406200));

    incoming.position_known = true;
    incoming.position_ms = 4000;
    player_sync_expect_track(&sync, &incoming, "test-device", "", true, 407000);
    CHECK(player_sync_waiting(&sync, &incoming, 407100));
    incoming.position_ms = 50;
    CHECK(!player_sync_waiting(&sync, &incoming, 407200));
}

static void test_local_seek_confirmation(void) {
    Playback expected;
    CHECK(playback_parse(PLAYER_FIXTURE, &expected));
    PlayerSeek seek = {0};
    player_seek_requested(&seek, 1, 1000);
    seek.local = true;
    seek.dispatched = true;
    seek.target_ms = 90000;
    seek.previous_anchor_ms = 100;
    player_seek_completed(&seek, 1, 1020, true);

    Playback incoming = expected;
    incoming.playing = false;
    incoming.buffering = true;
    incoming.position_ms = incoming.anchor_position_ms = 90000;
    incoming.anchor_timestamp_ms = 101;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 1100) == SEEK_WAITING);
    CHECK(seek.pending);

    incoming = expected;
    incoming.position_ms = 90040;
    incoming.anchor_position_ms = 89800;
    incoming.anchor_timestamp_ms = 100;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 1250) == SEEK_WAITING);
    incoming.anchor_timestamp_ms = 102;
    incoming.position_ms = 89999;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 1299) == SEEK_WAITING);
    incoming.position_ms = 90000;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 1300) == SEEK_READY);
    CHECK(!seek.pending);

    player_seek_requested(&seek, 2, 2000);
    seek.local = true;
    seek.target_ms = 34000;
    expected.playing = false;
    incoming = expected;
    incoming.anchor_timestamp_ms = 103;
    incoming.position_ms = incoming.anchor_position_ms = 34000;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 2100) == SEEK_WAITING);
    seek.dispatched = true;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 2300) == SEEK_READY);
    CHECK(!seek.pending && !incoming.playing && incoming.position_ms == 34000);

    player_seek_requested(&seek, 3, 3000);
    snprintf(incoming.uri, sizeof(incoming.uri), "spotify:track:different");
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 3100) == SEEK_READY);
    CHECK(!seek.pending);

    player_seek_requested(&seek, 4, 4000);
    seek.local = true;
    seek.dispatched = true;
    seek.target_ms = 180000;
    player_seek_completed(&seek, 4, 4020, true);
    incoming = expected;
    incoming.anchor_timestamp_ms = 104;
    incoming.anchor_position_ms = 170000;
    incoming.position_ms = 180000;
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 4100) == SEEK_WAITING);
    CHECK(player_seek_reconcile_local(&seek, &expected, &incoming, 6520) == SEEK_TIMED_OUT);
    CHECK(!seek.pending);

    CHECK(player_local_seek_due(0, false, 1000) == 1000);
    CHECK(player_local_seek_due(1000, false, 1010) == 1240);
    CHECK(player_local_seek_due(1000, true, 1220) == 1310);
    CHECK(player_local_seek_due(1000, false, 1500) == 1500);
}

static void test_seek_confirmation(void) {
    Playback expected;
    CHECK(playback_parse(PLAYER_FIXTURE, &expected));
    expected.position_ms = 90000;
    PlayerSeek seek = {0};
    player_seek_requested(&seek, 1, 1000);
    player_seek_completed(&seek, 1, 1200, true);
    CHECK(seek.pending && seek.deadline_ms == 6200);

    Playback incoming = expected;
    incoming.position_ms = 10000;
    CHECK(player_seek_reconcile(&seek, &expected, 1000, &incoming, 1500));
    CHECK(incoming.position_ms == 90500 && seek.pending);

    incoming = expected;
    incoming.position_ms = 90900;
    CHECK(player_seek_reconcile(&seek, &expected, 1000, &incoming, 2000));
    CHECK(!seek.pending && incoming.position_ms == 91000);

    player_seek_requested(&seek, 2, 2000);
    player_seek_requested(&seek, 3, 2100);
    player_seek_completed(&seek, 2, 2200, false);
    CHECK(seek.pending && seek.sequence == 3);
    player_seek_completed(&seek, 3, 2300, true);
    incoming = expected;
    incoming.position_ms = 10000;
    CHECK(player_seek_reconcile(&seek, &expected, 2100, &incoming, 7300));
    CHECK(!seek.pending && incoming.position_ms == 10000);

    player_seek_requested(&seek, 4, 8000);
    player_seek_completed(&seek, 4, 8100, false);
    CHECK(!seek.pending);

    player_seek_requested(&seek, 5, 9000);
    incoming = expected;
    incoming.has_device = false;
    CHECK(!player_seek_reconcile(&seek, &expected, 9000, &incoming, 9100));
    CHECK(seek.pending);

    incoming = expected;
    snprintf(incoming.uri, sizeof(incoming.uri), "spotify:track:different");
    CHECK(player_seek_reconcile(&seek, &expected, 9000, &incoming, 9200));
    CHECK(!seek.pending);

    expected.playing = false;
    player_seek_requested(&seek, 6, 10000);
    incoming = expected;
    incoming.position_ms = 10000;
    CHECK(player_seek_reconcile(&seek, &expected, 10000, &incoming, 11000));
    CHECK(incoming.position_ms == 90000 && seek.pending);
}

static void test_playback_options(void) {
    Playback playback;
    CHECK(playback_parse(PLAYER_FIXTURE, &playback));
    char url[1024];
    CHECK(player_options_url(&playback, true, 1, url, sizeof(url)));
    CHECK(strstr(url, "/shuffle?state=true&device_id=speaker%22%5C%26id") != NULL);
    CHECK(player_options_url(&playback, false, REPEAT_CONTEXT, url, sizeof(url)));
    CHECK(strstr(url, "/repeat?state=context&device_id=") != NULL);
    CHECK(player_options_url(&playback, false, REPEAT_TRACK, url, sizeof(url)));
    CHECK(strstr(url, "state=track") != NULL);
    CHECK(player_options_url(&playback, false, REPEAT_OFF, url, sizeof(url)));
    CHECK(strstr(url, "state=off") != NULL);
    CHECK(!player_options_url(&playback, true, 2, url, sizeof(url)));
    CHECK(!player_options_url(&playback, false, 3, url, sizeof(url)));
    playback.restricted = true;
    CHECK(!player_options_url(&playback, true, 1, url, sizeof(url)));
    CHECK(
        playback_parse("{\"is_playing\":true,\"shuffle_state\":true,\"repeat_state\":\"track\"}", &playback));
    CHECK(playback.shuffle && playback.repeat == REPEAT_TRACK);
}

static void test_soloist_state(void) {
    const char *state =
        "{\"type\":\"playback_state\",\"is_active\":true,\"status\":\"playing\",\"volume\":64,"
        "\"options\":{\"shuffle\":true,\"repeat\":\"context\"},"
        "\"context\":{\"uri\":\"spotify:album:6dVIqQ8qmQ5GBnJ9shOYGE\"},"
        "\"position\":{\"position_ms\":80000,\"timestamp_ms\":1000000,\"speed\":1.0},"
        "\"available_actions\":{\"seek\":{},\"skip_next\":{}},"
        "\"item\":{\"uri\":\"spotify:track:4uLU6hMCjMI75M1A2tKUQC\",\"entity_type\":\"track\",\"decorations\":{"
        "\"identity\":{\"name\":\"Let Down\"},\"playback\":{\"duration_ms\":299000},"
        "\"creators\":[{\"entity\":{\"uri\":\"spotify:artist:4Z8W4fKeB5YxbusRsdQVPb\","
        "\"decorations\":{\"identity\":{\"name\":\"Radiohead\"}}}}],"
        "\"parent\":{\"entity\":{\"uri\":\"spotify:album:6dVIqQ8qmQ5GBnJ9shOYGE\","
        "\"decorations\":{\"identity\":{\"name\":\"OK Computer\"}}}}}}}";
    Playback playback;

    // The anchor dates the position: 1.5 s later, a playing track has moved on by 1.5 s.
    CHECK(soloist_state_parse(state, "phone", "Native Spotify Phone", 1001500, &playback));
    CHECK(playback.playing && !playback.buffering && playback.position_known);
    CHECK(playback.position_ms == 81500 && playback.anchor_position_ms == 80000);
    CHECK(playback.anchor_timestamp_ms == 1000000 && playback.duration_ms == 299000);
    CHECK(!strcmp(playback.uri, "spotify:track:4uLU6hMCjMI75M1A2tKUQC") && !strcmp(playback.title, "Let Down"));
    CHECK(!strcmp(playback.artist, "Radiohead") && !strcmp(playback.artist_id, "4Z8W4fKeB5YxbusRsdQVPb"));
    CHECK(!strcmp(playback.album, "OK Computer") && !strcmp(playback.album_id, "6dVIqQ8qmQ5GBnJ9shOYGE"));
    CHECK(!strcmp(playback.device_id, "phone") && playback.has_device && !playback.restricted);
    CHECK(playback.shuffle && playback.repeat == REPEAT_CONTEXT && playback.volume_percent == 64);
    CHECK(!playback.seeking_disallowed && !playback.skipping_next_disallowed &&
          playback.skipping_previous_disallowed);

    // Buffering after a seek holds the clock at the target.
    const char *buffering = "{\"status\":\"buffering\",\"position\":{\"position_ms\":120000,"
                            "\"timestamp_ms\":1000000,\"speed\":0.0}}";
    CHECK(soloist_state_parse(buffering, "phone", "Native Spotify Phone", 1003000, &playback));
    CHECK(!playback.playing && playback.buffering && playback.position_ms == 120000);

    CHECK(!soloist_state_parse("{\"status\":\"unknown\"}", "phone", "Phone", 0, &playback));
}

int main(void) {
    test_devices();
    test_queue();
    test_playback_controls();
    test_no_session_requests();
    test_commands_and_sync();
    test_seek_confirmation();
    test_local_seek_confirmation();
    test_playback_options();
    test_soloist_state();

    printf("PASS: %u player checks (devices, queue, restrictions, transfer JSON, seek/volume URLs, drafts)\n",
           checks);
    return 0;
}
