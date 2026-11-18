#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "platform.h"
#include "player_fixture.h"
#include "ui/scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include "ui/linux_canvas.h"
#include <fontconfig/fontconfig.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include "ui/windows_canvas.h"
#else
#include <time.h>
#endif

static unsigned checks;

#define CHECK(expression)                                                                                    \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(expression)) {                                                                                 \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression);                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

/* Waits for background lanes to finish the list load; never longer than one second. */
static void wait_loaded(void) {
    for (unsigned i = 0; i < 200 && app_snapshot().list.loading; ++i) {
#ifdef _WIN32
        Sleep(5);
#else
        struct timespec interval = {0, 5000000};
        nanosleep(&interval, NULL);
#endif
    }

    CHECK(!app_snapshot().list.loading);
}

#ifndef _WIN32
static void fixture_list(View *view, const BrowseRequest *request, const BrowsePage *page) {
    view->list.request = *request;
    view->list.loaded = true;
    view->list.count = page->count;
    view->list.total = page->total;
    view->list.first = 0;
    view->list.rows_count = page->count;
    memcpy(view->list.rows, page->items, page->count * sizeof(page->items[0]));
}
#endif

static void test_no_session(void) {
    View snapshot = app_snapshot();
    CHECK(snapshot.engine == ENGINE_OFF && !snapshot.list.loaded && !snapshot.list.loading);
    CHECK(!app_browse(BROWSE_SEARCH, ""));
    CHECK(!app_select(0));
    CHECK(!app_activate());
    CHECK(!app_open_link());
    CHECK(!app_transfer());
    CHECK(!app_seek(500));
    CHECK(!app_volume(50));
    CHECK(!app_select_device(0));
    CHECK(!app_load_more());
    CHECK(!list_row(&snapshot.list, 0));

    // Commands are refused before any optimistic change when there is no session.
    unsigned revision = app_snapshot().playback_revision;
    app_action(ACTION_PLAY);
    CHECK(!app_snapshot().playback.playing && app_snapshot().playback_revision == revision);
    CHECK(!strcmp(app_snapshot().status, "Connect your Spotify account first."));

    app_action(ACTION_DEVICES);
    CHECK(!app_snapshot().devices.count);

    app_action(ACTION_QUEUE);
    wait_loaded();
    CHECK(!app_snapshot().queue.count);
    CHECK(!strcmp(app_snapshot().status, "Connect your Spotify account first."));

    // Invalid Soloist keys are rejected immediately and never reach the credential store.
    CHECK(!app_engine_key("short", 5));
    CHECK(strstr(app_snapshot().engine_message, "does not contain a Soloist key") != NULL);
}

static void test_layout(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;

    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.x == 8 && scene.sidebar.y == 64);
    CHECK(scene.content.x > scene.sidebar.x + scene.sidebar.w);
    CHECK(scene.player.y >= scene.content.y + scene.content.h);
    CHECK(scene.search.y == 8 && scene.search.h == 48);
    CHECK(scene_hit(&scene, scene.search.x + 60, 24) == HIT_SEARCH);
    CHECK(scene_hit(&scene, 0, 0) == HIT_NONE);
    CHECK(!scene.hits[HIT_SEEK].enabled && !scene.hits[HIT_VOLUME].enabled);

    for (unsigned i = 0; i < 4; ++i) {
        Rect rect = scene.hits[HIT_HOME_QUICK_FIRST + i].rect;
        CHECK(rect.w > 100 && rect.x >= scene.content.x &&
              rect.x + rect.w < scene.content.x + scene.content.w);
        CHECK(rect.y + rect.h < scene.player.y);
        CHECK(scene_hit(&scene, rect.x + 20, rect.y + 20) == HIT_HOME_QUICK_FIRST + i);
    }

    CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
    ++view.playback_revision;
    scene_layout(&scene, &view, 960, 700);
    CHECK(scene.hits[HIT_SEEK].enabled && scene.hits[HIT_VOLUME].enabled);
    CHECK(scene.draft.seek == 250 && scene.draft.volume == 65);
    CHECK(scene.content.w > 600);
}

static void test_input_and_activation(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    scene_layout(&scene, &view, 1280, 800);

    CHECK(scene_input(&scene, "Björk ♫", strlen("Björk ♫")));
    CHECK(scene_key(&scene, &view, KEY_BACKSPACE, false));
    CHECK(!strcmp(scene.query, "Björk "));
    scene.select_all = true;
    CHECK(scene_input(&scene, "Jóga", strlen("Jóga")));
    CHECK(!strcmp(scene.query, "Jóga"));
    CHECK(scene_key(&scene, &view, KEY_ENTER, false));
    wait_loaded();
    CHECK(!scene.home);
    CHECK(!strcmp(app_snapshot().status, "Connect your Spotify account first."));

    scene.home = true;
    scene_layout(&scene, &view, 1280, 800);
    Rect card = scene.hits[HIT_HOME_QUICK_FIRST + 1].rect;
    scene_press(&scene, &view, card.x + 20, card.y + 20, 1000);
    CHECK(scene.home);
    scene_release(&scene, &view, 0, 0);
    CHECK(scene.home);

    scene_press(&scene, &view, card.x + 20, card.y + 20, 1000);
    scene_release(&scene, &view, card.x + 20, card.y + 20);
    wait_loaded();
    CHECK(!scene.home && scene.section == BROWSE_SAVED_ALBUMS);

    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false));
    CHECK(scene.focus == HIT_NONE);

    ++view.session_revision;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.query[0] && scene.home && !scene.device_popup);
}

static void test_animation(void) {
    Scene scene;
    scene_init(&scene);
    scene_hover(&scene, HIT_HOME, 1000);
    CHECK(scene.animating);
    CHECK(scene_tick(&scene, 1016));
    CHECK(scene.hover[HIT_HOME] > 0 && scene.hover[HIT_HOME] < 1);

    for (unsigned i = 2; i < 80; ++i) {
        scene_tick(&scene, 1000 + i * 16);
    }

    CHECK(!scene.animating && scene.hover[HIT_HOME] == 1);
    CHECK(!scene_tick(&scene, 10000));

    scene_hover(&scene, HIT_NONE, 10000);
    for (unsigned i = 1; i < 80; ++i) {
        scene_tick(&scene, 10000 + i * 16);
    }
    CHECK(!scene.animating && scene.hover[HIT_HOME] == 0);

    scene.entrance = 1;
    scene.animating = true;
    scene.last_ms = 20000;
    scene_tick(&scene, 20032);
    CHECK(scene.entrance == 1 && !scene.animating);
    for (unsigned i = 2; i < 20; ++i) {
        scene_tick(&scene, 20000 + i * 32);
    }
    CHECK(scene.entrance == 1 && !scene.animating);

    scene.pending_browse = true;
    scene.entrance = 0;
    scene.animating = true;
    CHECK(!scene_tick(&scene, 30000));
    CHECK(scene.entrance == 0 && !scene.animating);
}

static void test_content_geometry(void) {
    Scene scene;
    scene_init(&scene);
    scene.home = false;
    scene.section = BROWSE_SAVED_ALBUMS;

    View view = app_snapshot();
    view.authenticated = true;
    view.list.loaded = true;
    view.list.request.kind = BROWSE_SAVED_ALBUMS;
    view.list.count = 30;
    scene_layout(&scene, &view, 1280, 800);

    // No page arrows: the grid uses the full height and scrolls instead.
    for (unsigned i = 0; i < 10; ++i) {
        Rect rect = scene.hits[HIT_ROW_FIRST + i].rect;
        CHECK(scene.hits[HIT_ROW_FIRST + i].enabled && rect.w > 100);
        CHECK(rect.y + rect.h <= scene.content.y + scene.content.h);
    }

    CHECK(!scene.hits[HIT_ROW_FIRST + 10].enabled);
    scene_scroll(&scene, &view, 1);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.first_row == 0 && scene.hits[HIT_ROW_FIRST].enabled && scene.hits[HIT_ROW_FIRST + 9].enabled);
    scene_scroll(&scene, &view, 1);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.first_row == 5 && scene.hits[HIT_ROW_FIRST + 9].enabled);

    for (unsigned i = 0; i < 10; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    // Strict end-of-list clamp: scrolling never leaves empty space below the last row.
    CHECK(scene.first_row == 15);
    scene_scroll(&scene, &view, -1);
    CHECK(scene.first_row == 10);
    scene.first_row = 0;
    scene.main_scroll = (Scroll){0};

    scene_layout(&scene, &view, 960, 700);
    scene_scroll(&scene, &view, 1);
    scene_scroll(&scene, &view, 1);
    scene_layout(&scene, &view, 960, 700);
    CHECK(scene.first_row == 4);
    CHECK(scene.hits[HIT_ROW_FIRST].enabled);

    view.list.selected = 12;
    scene_layout(&scene, &view, 960, 700);
    CHECK(scene.first_row <= 12 && scene.first_row % 4 == 0);
    CHECK(scene.hits[HIT_ROW_FIRST + (12 - scene.first_row)].enabled);

    // List view rows scroll a few at a time and fill tall windows without dropping top visible tracks.
    scene.section = BROWSE_SAVED_TRACKS;
    scene.first_row = 0;
    scene.main_scroll = (Scroll){0};
    view.list.selected = -1;
    view.list.request.kind = BROWSE_SAVED_TRACKS;
    scene_layout(&scene, &view, 1280, 1400);
    CHECK(scene.hits[HIT_ROW_FIRST + 15].enabled);
    scene_scroll(&scene, &view, 1);
    scene_layout(&scene, &view, 1280, 1400);
    CHECK(scene.first_row == 0 && scene.hits[HIT_ROW_FIRST].enabled);
    scene_scroll(&scene, &view, 1);
    CHECK(scene.first_row == 1);
    scene.section = BROWSE_SAVED_ALBUMS;
    view.list.request.kind = BROWSE_SAVED_ALBUMS;
    scene.first_row = 0;
    scene.main_scroll = (Scroll){0};

    scene.device_popup = true;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.device_panel.y > 64 && scene.device_panel.y + scene.device_panel.h < scene.player.y);
    CHECK(scene_hit(&scene, scene.device_panel.x + 10, scene.device_panel.y + 60) == HIT_NONE);

    // Without a ready Soloist the device button opens its setup panel, which is modal.
    CHECK(scene.hits[HIT_TRANSFER].enabled);
    scene_activate(&scene, &view, HIT_TRANSFER);
    CHECK(scene.setup_popup && !scene.device_popup);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_SETUP_PASTE].enabled && scene.hits[HIT_SETUP_KEY].enabled);
    CHECK(!scene.hits[HIT_SETUP_FORGET].enabled);
    Rect paste = scene.hits[HIT_SETUP_PASTE].rect;
    CHECK(scene_hit(&scene, paste.x + 10, paste.y + 10) == HIT_SETUP_PASTE);
    CHECK(scene_hit(&scene, scene.search.x + 60, 24) == HIT_NONE);
    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false) && !scene.setup_popup);

    // The app requests the panel by bumping a revision (e.g. playing a track before setup).
    ++view.setup_revision;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.setup_popup);
    scene.setup_popup = false;

    view.engine = ENGINE_READY;
    scene.device_popup = true;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.hits[HIT_TRANSFER].enabled);
    view.engine = ENGINE_OFF;

    CHECK(devices_parse(DEVICES_FIXTURE, &view.devices));
    view.devices.count = DEVICE_LIMIT;
    view.devices.selected = DEVICE_LIMIT - 1;
    ++view.devices_revision;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.device_first == DEVICE_LIMIT - 8);
    CHECK(scene.hits[HIT_DEVICE_LAST].enabled);
    scene_scroll(&scene, &view, -1);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.hits[HIT_DEVICE_LAST].enabled);

    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.device_panel.y >= 64);
    CHECK(scene.device_panel.y + scene.device_panel.h < scene.player.y);
    CHECK(scene.device_visible == 8);

    scene.device_popup = false;
    scene.home = true;
    scene_layout(&scene, &view, 960, 640);

    for (unsigned i = 0; i < 4; ++i) {
        Rect rect = scene.hits[HIT_HOME_QUICK_FIRST + i].rect;
        CHECK(rect.w > 100 && rect.y + rect.h <= scene.content.y + scene.content.h - 60);
    }

    CHECK(color_mix(0, 0xffffff, 0) == 0);
    CHECK(color_mix(0, 0xffffff, 1) == 0xffffff);
    CHECK(color_mix(0, 0xffffff, .5f) == 0x7f7f7f);
}

static void test_player_drag(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
    ++view.playback_revision;
    scene_layout(&scene, &view, 1280, 800);

    Rect seek = scene.hits[HIT_SEEK].rect;
    scene_press(&scene, &view, seek.x + 10, seek.y + 5, 1000);
    CHECK(scene.dragging == HIT_SEEK);
    scene_motion(&scene, seek.x + seek.w + 200);
    CHECK(scene.draft.seek == 1000);
    scene_release(&scene, &view, seek.x + seek.w, seek.y);
    CHECK(scene.dragging == HIT_NONE);

    view.playback.restricted = true;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.hits[HIT_SEEK].enabled && !scene.hits[HIT_VOLUME].enabled);
}

static void test_login_screen(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.login_screen && scene.focus == HIT_ACCOUNT);
    CHECK(scene.hits[HIT_ACCOUNT].enabled);
    CHECK(!scene.hits[HIT_ROW_FIRST].enabled && !scene.hits[HIT_CARD_FIRST].enabled);
    CHECK(scene.login_panel.x + scene.login_panel.w / 2 == 640);
    CHECK(scene.login_panel.y + scene.login_panel.h / 2 == 400);
    CHECK(scene.hits[HIT_ACCOUNT].rect.x + scene.hits[HIT_ACCOUNT].rect.w / 2 == 640);
    CHECK(!scene.hits[HIT_SEARCH].enabled && !scene.hits[HIT_VOLUME].enabled);
    CHECK(client_id_valid(NATIVE_SPOTIFY_CLIENT_ID));
    CHECK(app_tick(false) == -1 && app_tick(true) == -1);

    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.login_panel.x + scene.login_panel.w / 2 == 480);
    CHECK(scene.login_panel.y + scene.login_panel.h / 2 == 320);
    view.connecting = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.hits[HIT_ACCOUNT].enabled);
    view.connecting = false;
    view.authenticated = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.login_screen && scene.focus == HIT_SEARCH);

    view.library[1].count = 1;
    view.library[2].count = 2;
    CHECK(scene_home_item(&view, 0) == &view.library[1].items[0]);
    CHECK(scene_home_item(&view, 1) == NULL);
    CHECK(scene_home_item(&view, 5) == &view.library[2].items[0]);
    CHECK(scene_home_item(&view, 6) == &view.library[2].items[1]);
}

static void test_search_and_library_geometry(void) {
    Scene scene;
    scene_init(&scene);
    scene.home = false;
    scene.section = BROWSE_SEARCH;
    View view = app_snapshot();
    view.authenticated = true;
    view.list.loaded = true;
    view.list.request.kind = BROWSE_SEARCH;
    view.list.count = 30;
    view.list.rows_count = 30;
    view.search_artist_count = 5;
    view.search_album_count = 5;
    view.playlists.count = 100;
    view.playlists.rows_count = LIST_WINDOW;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_SEARCH_ALL].enabled && scene.hits[HIT_SEARCH_ALBUMS].enabled);
    CHECK(scene.hits[HIT_SEARCH_TOP].rect.x < scene.hits[HIT_ROW_FIRST].rect.x);
    CHECK(scene.hits[HIT_ROW_FIRST + 3].enabled && !scene.hits[HIT_ROW_FIRST + 4].enabled);
    CHECK(scene.hits[HIT_SEARCH_ARTIST_FIRST].enabled && scene.hits[HIT_SEARCH_ALBUM_FIRST].enabled);
    scene_scroll(&scene, &view, 1);
    CHECK(scene.search_scroll == 96 && scene.first_row == 0);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene_hit(&scene, scene.content.x + 40, scene.content.y + 20) == HIT_SEARCH_ALL);

    scene.search_scroll = 0;
    scene.main_scroll = (Scroll){0};
    scene.search_filter = SEARCH_ARTISTS;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_ROW_FIRST + 4].enabled);
    scene_scroll(&scene, &view, 1);
    CHECK(scene.first_row == 0);
    scene_scroll(&scene, &view, 1);
    CHECK(scene.first_row == 5);

    CHECK(scene.hits[HIT_LIBRARY_FIRST + 4].enabled);
    scene.over_library = true;
    scene_scroll(&scene, &view, 1);
    CHECK(scene.library_first == 3 && scene.first_row == 5);

    for (unsigned i = 0; i < 50; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    CHECK(scene.library_first == 90);
    scene_scroll(&scene, &view, -1);
    CHECK(scene.library_first == 87);
    scene.setup_popup = true;
    scene_scroll(&scene, &view, -1);
    CHECK(scene.library_first == 87);
    app_viewport(0);
    app_library_viewport(0);
}

static void test_collection_clicks_do_not_play(void) {
    CatalogItem artist = {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijkl", .title = "Fixture artist"};
    unsigned revision = app_snapshot().playback_revision;
    CHECK(app_open_item(&artist));
    wait_loaded();
    CHECK(app_snapshot().list.request.kind == BROWSE_ARTIST_ALBUMS);
    CHECK(app_snapshot().playback_revision == revision);

    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    view.search_artists[0] = artist;
    view.search_artist_count = 1;
    scene.home = false;
    scene.section = BROWSE_SEARCH;
    scene_layout(&scene, &view, 1280, 800);
    scene_activate(&scene, &view, HIT_SEARCH_TOP);
    wait_loaded();
    CHECK(scene.section == BROWSE_ARTIST_ALBUMS && !scene.home);
    CHECK(!strcmp(scene.collection, "Fixture artist"));
    CHECK(app_snapshot().playback_revision == revision);

    CatalogItem album = {.kind = ITEM_ALBUM, .id = "0123456789abcdefghijkl", .title = "Fixture album"};
    CHECK(app_open_item(&album));
    wait_loaded();
    CHECK(app_snapshot().list.request.kind == BROWSE_ALBUM_TRACKS);
    CHECK(app_snapshot().playback_revision == revision);
}

static void test_home_scrolling_and_panels(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    view.recent_count = 8;
    view.library[1].count = 10;
    view.library[2].count = 10;
    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.home_extent > scene.content.h);
    CHECK(scene.hits[HIT_HOME_QUICK_FIRST + 7].rect.w > 100);
    CHECK(scene.hits[HIT_CARD_FIRST].rect.h == scene.hits[HIT_CARD_FIRST].rect.w + 72);

    for (unsigned i = 0; i < 40; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    scene_layout(&scene, &view, 960, 640);
    Rect bottom = scene.hits[HIT_CARD_FIRST + 8].rect;
    CHECK(bottom.w > 100 && bottom.y + bottom.h <= scene.content.y + scene.content.h - 16);
    CHECK(scene_hit(&scene, bottom.x + 16, scene.player.y + 4) != HIT_CARD_FIRST + 8);
    scene.home_scroll = 0;
    scene.main_scroll = (Scroll){0};

    scene_activate(&scene, &view, HIT_FRIENDS);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_FRIENDS && scene.side_panel.x >= scene.content.x);
    CHECK(scene_hit(&scene, scene.side_panel.x + 20, scene.side_panel.y + 180) == HIT_NONE);
    scene_activate(&scene, &view, HIT_FRIENDS);
    CHECK(scene.panel == PANEL_NONE);
    scene_activate(&scene, &view, HIT_LYRICS);
    CHECK(scene.panel == PANEL_LYRICS);
    scene_activate(&scene, &view, HIT_NOW_PLAYING);
    scene_layout(&scene, &view, 1600, 900);
    CHECK(scene.panel == PANEL_NOW_PLAYING);
    CHECK(scene.content.x + scene.content.w + 8 <= scene.side_panel.x);
    scene_activate(&scene, &view, HIT_PANEL_CLOSE);
    scene_layout(&scene, &view, 1600, 900);
    CHECK(scene.panel == PANEL_NONE && scene.content.w > 1200);

    view.queue.count = 20;
    scene_activate(&scene, &view, HIT_QUEUE);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_QUEUE && scene.hits[HIT_PANEL_QUEUE].enabled);
    scene.over_panel = true;

    for (unsigned i = 0; i < 20; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    CHECK(scene.panel_first == 12 && scene.home_scroll == 0);
    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false));
    CHECK(scene.panel == PANEL_NONE);

    // Wheel over volume adjusts volume by 5% per step instead of scrolling the list.
    CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
    ++view.playback_revision;
    scene_layout(&scene, &view, 1280, 800);
    scene.hot = HIT_VOLUME;
    unsigned before_vol = scene.draft.volume;
    scene_scroll(&scene, &view, -1);
    CHECK(scene.draft.volume == before_vol + 5 && scene.main_scroll.offset == 0);
    scene_scroll(&scene, &view, 1);
    CHECK(scene.draft.volume == before_vol);
    scene.hot = HIT_NONE;

    // Draggable scrollbars for main content, library and queue panel.
    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.hits[HIT_SCROLL_MAIN].enabled);
    Rect main_thumb = scene.hits[HIT_SCROLL_MAIN].rect;
    scene_press(&scene, &view, main_thumb.x + 2, main_thumb.y + 4, 2000);
    CHECK(scene.dragging == HIT_SCROLL_MAIN);
    scene_pointer_motion(&scene, main_thumb.x + 2, main_thumb.y + 120);
    CHECK(scene.main_scroll.offset > 0 && scene.home_scroll == scene.main_scroll.offset);
    scene_release(&scene, &view, main_thumb.x + 2, main_thumb.y + 120);
    CHECK(scene.dragging == HIT_NONE);

    // Smooth scrolling settles within 100ms and stops animating when settled.
    scene.main_scroll = (Scroll){0};
    scene_layout(&scene, &view, 960, 640);
    scene.last_ms = 5000;
    scene_scroll(&scene, &view, 1);
    CHECK(scene.main_scroll.active && scene.animating);
    CHECK(scene_tick(&scene, 5050));
    CHECK(scene.main_scroll.offset > 0 && scene.main_scroll.offset < 96);
    CHECK(!scene_tick(&scene, 5120));
    CHECK(!scene.main_scroll.active && !scene.animating && scene.main_scroll.offset == 96);

    // Detailed album, artist and profile views.
    scene.home = false;
    scene.section = BROWSE_ALBUM_TRACKS;
    scene.main_scroll = (Scroll){0};
    view.details = (CollectionInfo){
        .loaded = true,
        .total_tracks = 2,
        .duration_ms = 441000,
        .duration_complete = true,
        .release_precision = "day",
        .label = "Apple Records",
        .copyrights = "© 2019 Apple",
        .item = {.kind = ITEM_ALBUM,
                 .id = "0123456789abcdefghijkl",
                 .title = "Abbey Road",
                 .release_date = "1969-09-26",
                 .album_type = "album"},
        .artist = {.kind = ITEM_ARTIST, .id = "abcdefghijkl0123456789", .title = "The Beatles"},
    };
    view.list.count = 2;
    view.list.total = 2;
    view.list.rows_count = 2;
    view.list.rows[0] = (CatalogItem){.kind = ITEM_TRACK,
                                      .id = "0123456789abcdefghijkl",
                                      .title = "Come Together",
                                      .detail = "The Beatles",
                                      .track_number = 1,
                                      .disc_number = 1,
                                      .duration_ms = 259000,
                                      .duration_known = true,
                                      .playable = true};
    view.list.rows[1] = view.list.rows[0];
    view.list.rows[1].track_number = 2;
    view.list.rows[1].explicit_content = true;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_DETAIL_PLAY].enabled && scene.hits[HIT_DETAIL_SAVE].enabled &&
          scene.hits[HIT_DETAIL_ARTIST].enabled && scene.hits[HIT_ROW_FIRST].enabled);

    scene_activate(&scene, &view, HIT_DETAIL_ARTIST);
    wait_loaded();
    CHECK(scene.section == BROWSE_ARTIST_ALBUMS);

    view.details.item.kind = ITEM_ARTIST;
    view.details.followers_known = true;
    view.details.followers = 28500000;
    view.details.popular_loaded = true;
    view.details.popular_count = 6;
    for (unsigned i = 0; i < 6; ++i) {
        view.details.popular[i] = view.list.rows[0];
    }
    scene_layout(&scene, &view, 1280, 1100);
    CHECK(scene.hits[HIT_POPULAR_FIRST + 4].enabled && !scene.hits[HIT_POPULAR_FIRST + 5].enabled);
    CHECK(scene.hits[HIT_DETAIL_ALL].enabled && scene.hits[HIT_DISC_ALL].enabled);
    scene_activate(&scene, &view, HIT_DETAIL_ALL);
    scene_layout(&scene, &view, 1280, 1100);
    CHECK(scene.popular_expanded && scene.hits[HIT_POPULAR_FIRST + 5].enabled);

    CHECK(!scene.hits[HIT_FRIENDS].enabled && !scene.hits[HIT_ACCOUNT].enabled &&
          scene.hits[HIT_PROFILE].enabled);
    scene_activate(&scene, &view, HIT_PROFILE);
    scene_layout(&scene, &view, 1280, 1100);
    CHECK(scene.menu.open && scene.menu.count == 2 && !strcmp(scene.menu.labels[0], "Profile") &&
          !strcmp(scene.menu.labels[1], "Log out"));
    scene_activate(&scene, &view, HIT_CTX_FIRST);
    wait_loaded();
    CHECK(!scene.menu.open && scene.section == BROWSE_PROFILE_PLAYLISTS);
    view.profile = (ProfileInfo){.loaded = true,
                                 .id = "user123",
                                 .name = "Spotify Listener",
                                 .followers = 12,
                                 .followers_known = true,
                                 .following_count = 1,
                                 .following_total = 1,
                                 .following_known = true};
    view.profile.following[0] = view.details.artist;
    scene_layout(&scene, &view, 1280, 900);
    CHECK(scene.hits[HIT_PROFILE_FOLLOWING].enabled && scene.hits[HIT_PROFILE_ARTIST_FIRST].enabled);

    // Right-click context menu on Now Playing bar and artist cards.
    Rect artist_card = scene.hits[HIT_PROFILE_ARTIST_FIRST].rect;
    scene_context_press(&scene, &view, artist_card.x + 12, artist_card.y + 12);
    scene_layout(&scene, &view, 1280, 900);
    CHECK(scene.menu.open && scene.menu.count >= 2 && scene.hits[HIT_CTX_FIRST].enabled);
    CHECK(scene_hit(&scene, scene.hits[HIT_CTX_FIRST].rect.x + 8, scene.hits[HIT_CTX_FIRST].rect.y + 8) ==
          HIT_CTX_FIRST);
    scene_activate(&scene, &view, HIT_CTX_FIRST);
    wait_loaded();
    CHECK(!scene.menu.open && scene.section == BROWSE_ARTIST_ALBUMS);

    snprintf(view.playback.uri, sizeof(view.playback.uri), "spotify:track:0123456789abcdefghijkl");
    snprintf(view.playback.artist_id, sizeof(view.playback.artist_id), "abcdefghijkl0123456789");
    snprintf(view.playback.album_id, sizeof(view.playback.album_id), "0123456789abcdefghijkl");
    scene_context_press(&scene, &view, 40, scene.player.y + 20);
    scene_layout(&scene, &view, 1280, 900);
    CHECK(scene.menu.open && scene.menu.count >= 4);
    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false) && !scene.menu.open);
}

static void check_text_center(const uint32_t *pixels, unsigned stride, Rect rect, bool dark) {
    unsigned left = (unsigned)(rect.x + rect.w);
    unsigned right = (unsigned)rect.x;
    unsigned top = (unsigned)(rect.y + rect.h);
    unsigned bottom = (unsigned)rect.y;

    for (unsigned y = (unsigned)rect.y + 6; y < (unsigned)(rect.y + rect.h) - 6; ++y) {
        for (unsigned x = (unsigned)rect.x + 24; x < (unsigned)(rect.x + rect.w) - 24; ++x) {
            uint32_t pixel = pixels[y * stride + x] & 0xffffff;
            bool ink = dark ? pixel < 0x404040 : pixel > 0xeeeeee;
            if (!ink) {
                continue;
            }

            if (x < left) {
                left = x;
            }
            if (x > right) {
                right = x;
            }
            if (y < top) {
                top = y;
            }
            if (y > bottom) {
                bottom = y;
            }
        }
    }

    CHECK(left <= right && top <= bottom);
    CHECK(abs((int)(left + right + 1) - (int)(rect.x * 2 + rect.w)) <= 2);
    CHECK(abs((int)(top + bottom + 1) - (int)(rect.y * 2 + rect.h)) <= 2);
}

static void test_label_alignment(void) {
    static const float scales[] = {1, 1.25f, 1.5f, 2};
    static const unsigned heights[] = {24, 32, 40, 48, 56, 64};
    static const char *labels[] = {"Play",  "Music",       "Log out", "Continue with Spotify",
                                   "Björk", "Quick glyphs"};

#ifdef _WIN32
    windows_load_font();
#else
    linux_load_font();
#endif

    for (unsigned scale = 0; scale < sizeof(scales) / sizeof(scales[0]); ++scale) {
        unsigned width = (unsigned)(400 * scales[scale]);
        unsigned height = (unsigned)(96 * scales[scale]);
        Canvas canvas;

#ifdef _WIN32
        WindowsCanvas backend = {0};
        HDC paint = CreateCompatibleDC(NULL);
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = (LONG)width;
        info.bmiHeader.biHeight = -(LONG)height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *data = NULL;
        HBITMAP image = CreateDIBSection(paint, &info, DIB_RGB_COLORS, &data, NULL, 0);
        CHECK(paint && image && data);
        HGDIOBJ previous = SelectObject(paint, image);
        uint32_t *pixels = data;
        unsigned stride = width;
        windows_canvas_begin(&backend, paint, scales[scale], &canvas);
#else
        LinuxCanvas backend = {0};
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)width, (int)height);
        cairo_t *paint = cairo_create(surface);
        cairo_scale(paint, scales[scale], scales[scale]);
        linux_canvas_begin(&backend, paint, &canvas);
        uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
        unsigned stride = (unsigned)cairo_image_surface_get_stride(surface) / 4;
#endif

        for (unsigned label = 0; label < sizeof(labels) / sizeof(labels[0]); ++label) {
            Rect rect = {40, 16, 320, (float)heights[label]};
            canvas.box(canvas.context, (Rect){0, 0, 400, 96}, 0, 0);
            canvas.box(canvas.context, rect, 0, 0xffffff);
            canvas.text(canvas.context, labels[label], rect, 14, TEXT_BOLD | TEXT_CENTER | TEXT_VCENTER, 0);

#ifdef _WIN32
            GdiFlush();
#else
            cairo_surface_flush(surface);
#endif

            check_text_center(pixels, stride,
                              (Rect){rect.x * scales[scale], rect.y * scales[scale], rect.w * scales[scale],
                                     rect.h * scales[scale]},
                              true);
        }

#ifdef _WIN32
        windows_canvas_destroy(&backend);
        SelectObject(paint, previous);
        DeleteObject(image);
        DeleteDC(paint);
#else
        linux_canvas_end(&backend);
        linux_canvas_destroy(&backend);
        cairo_destroy(paint);
        cairo_surface_destroy(surface);
#endif
    }
}

#ifndef _WIN32
static void test_navigation_rendering(void) {
    LinuxCanvas backend = {0};
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    snprintf(view.status, sizeof(view.status), "OFFLINE TEST FIXTURE · Navigation verification");
    scene_layout(&scene, &view, 1280, 800);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1280, 800);
    cairo_t *paint = cairo_create(surface);
    Canvas canvas;
    linux_canvas_begin(&backend, paint, &canvas);
    scene_draw(&scene, &view, &canvas);
    cairo_surface_flush(surface);
    unsigned stride = (unsigned)cairo_image_surface_get_stride(surface) / 4;
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
    uint32_t *outgoing = malloc(stride * 800 * sizeof(*outgoing));
    CHECK(outgoing != NULL);
    memcpy(outgoing, pixels, stride * 800 * sizeof(*outgoing));

    scene_activate(&scene, &view, HIT_TRACKS);
    wait_loaded();
    CHECK(!scene.retain_content && scene.pending_browse && scene.entrance == 1);
    view.list.loading = true;
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, &canvas);
    CHECK(backend.retained == NULL && !scene.retain_content);
    CHECK(!scene_tick(&scene, scene.last_ms + 2000));
    CHECK(!scene.animating && scene.entrance == 1);
    CHECK(!scene.hits[HIT_ROW_FIRST].enabled && !scene.hits[HIT_OPEN].enabled);
    unsigned sample = ((unsigned)scene.content.y + 30) * stride + (unsigned)scene.content.x + 30;
    uint32_t loaded = 0;

    for (unsigned frame = 0; frame < 4; ++frame) {
        if (frame == 1) {
            view.list.loading = false;
            view.list.loaded = true;
            view.list.request.kind = BROWSE_SAVED_TRACKS;
            scene_layout(&scene, &view, 1280, 800);
            CHECK(!scene.pending_browse && !scene.animating && scene.entrance == 1);
        }

        if (frame == 2) {
            scene_tick(&scene, scene.last_ms + 45);
            scene_tick(&scene, scene.last_ms + 45);
            CHECK(scene.entrance == 1);
        }

        if (frame == 3) {
            for (unsigned tick = 0; tick < 4; ++tick) {
                scene_tick(&scene, scene.last_ms + 45);
            }

            CHECK(!scene.animating && scene.entrance == 1);
        }

        scene_draw(&scene, &view, &canvas);
        cairo_surface_flush(surface);
        bool unchanged = true;

        for (unsigned y = (unsigned)scene.content.y + 8;
             y < (unsigned)(scene.content.y + scene.content.h) - 8; ++y) {
            unchanged = unchanged && !memcmp(pixels + y * stride + (unsigned)scene.content.x + 8,
                                             outgoing + y * stride + (unsigned)scene.content.x + 8,
                                             ((unsigned)scene.content.w - 16) * sizeof(*pixels));
        }

        CHECK(!unchanged);

        if (frame == 1) {
            loaded = pixels[sample];
        }

        if (frame == 3) {
            CHECK(pixels[sample] == loaded);
        }

        const char *directory = getenv("NATIVE_RENDER_OUTPUT");

        if (directory) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/native-transition-%u-fixture.png", directory, frame);
            CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
        }
    }

    CHECK(backend.retained == NULL);
    scene_activate(&scene, &view, HIT_HOME);
    CHECK(!scene.retain_content && scene.entrance == 1);
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, &canvas);
    CHECK(backend.retained == NULL);
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.retain_content && scene.entrance == 1);
    scene_draw(&scene, &view, &canvas);
    CHECK(backend.retained == NULL);
    ++view.session_revision;
    scene.pending_browse = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.pending_browse && scene.home);

    free(outgoing);
    linux_canvas_end(&backend);
    linux_canvas_destroy(&backend);
    cairo_destroy(paint);
    cairo_surface_destroy(surface);
}

static void test_rendering(void) {
    linux_load_font();
    LinuxCanvas backend = {0};
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    snprintf(view.status, sizeof(view.status),
             "OFFLINE TEST FIXTURE · Renderer verification, not Spotify account data.");
    BrowseRequest request = {.kind = BROWSE_SAVED_TRACKS};
    BrowsePage page;
    CHECK(browse_parse(
        "{\"offset\":0,\"limit\":10,\"total\":1,\"next\":null,\"items\":[{\"track\":" QUEUE_TRACK "}]}",
        &request, &page));
    fixture_list(&view, &request, &page);
    view.library[0] = page;
    view.library_loaded[0] = true;
    BrowseRequest albums = {.kind = BROWSE_SAVED_ALBUMS};
    CHECK(browse_parse("{\"offset\":0,\"limit\":10,\"total\":1,\"next\":null,\"items\":[{\"album\":{\"type\":"
                       "\"album\",\"id\":\"0123456789abcdefghijkl\",\"name\":\"Fixture "
                       "album\",\"artists\":[{\"name\":\"Offline renderer\"}]}}]}",
                       &albums, &view.library[1]));
    BrowseRequest playlists = {.kind = BROWSE_PLAYLISTS};
    CHECK(browse_parse("{\"offset\":0,\"limit\":10,\"total\":1,\"next\":null,\"items\":[{\"type\":"
                       "\"playlist\",\"id\":\"0123456789abcdefghijkl\",\"name\":\"Fixture "
                       "playlist\",\"owner\":{\"display_name\":\"Offline renderer\"}}]}",
                       &playlists, &view.library[2]));
    CHECK(devices_parse(DEVICES_FIXTURE, &view.devices));

    view.playlists.loaded = true;
    view.playlists.count = 30;
    view.playlists.rows_count = 30;

    for (unsigned i = 0; i < 30; ++i) {
        view.playlists.rows[i] = view.library[2].items[0];
        snprintf(view.playlists.rows[i].title, sizeof(view.playlists.rows[i].title), "Fixture playlist %u",
                 i + 1);
    }

    for (unsigned state = 0; state < 19; ++state) {
        unsigned width = state == 9                                               ? 1600
                         : state == 2 || state == 5 || state == 10 || state == 11 ? 960
                                                                                  : 1280;
        unsigned height = state == 9                                               ? 900
                          : state == 2 || state == 5 || state == 10 || state == 11 ? 640
                                                                                   : 800;
        scene.home = state == 3 || (state >= 9 && state <= 15);
        scene.home_scroll = state == 11 ? UINT32_MAX : 0;
        scene.main_scroll.offset = state == 11 ? 100000.0f : state == 8 ? 340.0f : 0;
        scene.main_scroll.target = scene.main_scroll.offset;
        scene.panel = state == 12   ? PANEL_NOW_PLAYING
                      : state == 13 ? PANEL_FRIENDS
                      : state == 14 ? PANEL_LYRICS
                      : state == 15 ? PANEL_QUEUE
                                    : PANEL_NONE;
        scene.device_popup = state == 4;
        scene.setup_popup = state == 6;
        view.engine = state == 6 ? ENGINE_NEEDS_KEY : ENGINE_OFF;
        scene.section = state == 7 || state == 8 ? BROWSE_SEARCH
                        : state == 16            ? BROWSE_ALBUM_TRACKS
                        : state == 17            ? BROWSE_ARTIST_ALBUMS
                        : state == 18            ? BROWSE_PROFILE_PLAYLISTS
                                                 : BROWSE_SAVED_TRACKS;
        scene.search_scroll = state == 8 ? 340 : 0;

        if (state >= 16) {
            view.details = (CollectionInfo){
                .loaded = true,
                .total_tracks = 4,
                .duration_ms = 842000,
                .duration_complete = true,
                .followers = 28500000,
                .followers_known = true,
                .release_precision = "day",
                .label = "Apple Records",
                .copyrights = "© 2019 Apple Corps Ltd.\n℗ 2019 Calderstone Productions Limited",
                .item = {.kind = state == 17 ? ITEM_ARTIST : ITEM_ALBUM,
                         .id = "0123456789abcdefghijkl",
                         .detail = "The Beatles",
                         .release_date = "1969-09-26",
                         .album_type = "album"},
                .artist = {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijkl", .title = "The Beatles"},
                .popular_loaded = true,
                .popular_count = 5,
            };
            snprintf(view.details.item.title, sizeof(view.details.item.title), "%s",
                     state == 17 ? "The Beatles" : "Abbey Road (Remastered)");

            for (unsigned i = 0; i < 5; ++i) {
                view.details.popular[i] = page.items[0];
                view.details.popular[i].duration_ms = 185000 + i * 17000;
                view.details.popular[i].duration_known = true;
                view.details.popular[i].explicit_content = i == 2;
                snprintf(view.details.popular[i].title, sizeof(view.details.popular[i].title),
                         "Here Comes The Sun %u", i + 1);
            }

            view.list.count = 4;
            view.list.total = 4;
            view.list.rows_count = 4;

            for (unsigned i = 0; i < 4; ++i) {
                view.list.rows[i] = state == 16 ? view.details.popular[i] : view.library[1].items[0];
                view.list.rows[i].track_number = i + 1;
                view.list.rows[i].disc_number = i >= 2 ? 2 : 1;
                snprintf(view.list.rows[i].release_date, sizeof(view.list.rows[i].release_date), "1969");
                snprintf(view.list.rows[i].album_type, sizeof(view.list.rows[i].album_type), "Album");
            }

            view.profile = (ProfileInfo){.loaded = true,
                                         .id = "user123",
                                         .name = "Native Listener",
                                         .followers = 18,
                                         .followers_known = true,
                                         .following_count = 3,
                                         .following_total = 3,
                                         .following_known = true};
            for (unsigned i = 0; i < 3; ++i) {
                view.profile.following[i] = view.details.artist;
                snprintf(view.profile.following[i].title, sizeof(view.profile.following[i].title),
                         "Followed artist %u", i + 1);
            }
        }

        if (state == 7 || state == 8) {
            view.list.request.kind = BROWSE_SEARCH;
            view.list.count = 4;
            view.list.rows_count = 4;
            view.search_artist_count = 5;
            view.search_album_count = 5;
            snprintf(scene.query, sizeof(scene.query), "label:stereofox");

            for (unsigned i = 0; i < 5; ++i) {
                view.search_artists[i] = (CatalogItem){.kind = ITEM_ARTIST, .id = "0123456789abcdefghijkl"};
                snprintf(view.search_artists[i].title, sizeof(view.search_artists[i].title),
                         "Fixture artist %u", i + 1);
                view.search_albums[i] = view.library[1].items[0];

                if (i < 4) {
                    view.list.rows[i] = page.items[0];
                    view.list.rows[i].duration_ms = 142000;
                    snprintf(view.list.rows[i].title, sizeof(view.list.rows[i].title), "Fixture song %u",
                             i + 1);
                    snprintf(view.list.rows[i].detail, sizeof(view.list.rows[i].detail), "Offline renderer");
                }
            }
        }

        if (state >= 9) {
            view.recent_count = 8;
            view.library[1].count = 5;
            view.library[2].count = 5;

            for (unsigned i = 0; i < 8; ++i) {
                view.recent[i] = view.library[1].items[0];
                snprintf(view.recent[i].title, sizeof(view.recent[i].title), "Fixture recent album %u",
                         i + 1);

                if (i < 5) {
                    view.library[1].items[i] = view.recent[i];
                    view.library[2].items[i] = view.library[2].items[0];
                }
            }

            CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
            view.queue.count = 20;

            for (unsigned i = 0; i < view.queue.count; ++i) {
                view.queue.items[i] = page.items[0];
                snprintf(view.queue.items[i].title, sizeof(view.queue.items[i].title),
                         "Fixture queue song %u", i + 1);
            }
        }
        view.authenticated = state != 1 && state != 2;
        scene_layout(&scene, &view, (float)width, (float)height);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)width, (int)height);
        cairo_t *paint = cairo_create(surface);
        Canvas canvas;
        linux_canvas_begin(&backend, paint, &canvas);
        scene_draw(&scene, &view, &canvas);
        linux_canvas_end(&backend);
        cairo_destroy(paint);
        cairo_surface_flush(surface);
        CHECK(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS);
        uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
        unsigned stride = (unsigned)cairo_image_surface_get_stride(surface) / 4;
        Rect panel = scene.login_screen ? scene.login_panel : scene.content;
        CHECK((pixels[(unsigned)panel.y * stride + (unsigned)panel.x] & 0xffffff) == 0);
        CHECK((pixels[((unsigned)(panel.y + panel.h) - 1) * stride + (unsigned)panel.x] & 0xffffff) == 0);
        CHECK((pixels[(unsigned)panel.y * stride + (unsigned)(panel.x + panel.w) - 1] & 0xffffff) == 0);
        CHECK((pixels[((unsigned)(panel.y + panel.h) - 1) * stride + (unsigned)(panel.x + panel.w) - 1] &
               0xffffff) == 0);

        if (scene.login_screen) {
            check_text_center(pixels, stride, scene.hits[HIT_ACCOUNT].rect, true);
            check_text_center(pixels, stride, (Rect){panel.x + 32, panel.y + 136, panel.w - 64, 48}, false);
        }

        if (scene.device_popup) {
            check_text_center(pixels, stride, scene.hits[HIT_TRANSFER].rect, true);
        }

        if (scene.setup_popup) {
            check_text_center(pixels, stride, scene.hits[HIT_SETUP_PASTE].rect, true);
        }

        bool clean = true;

        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 50; ++x) {
                clean = clean && (pixels[y * stride + x] & 0xffffff) == 0;
            }
        }

        CHECK(clean);
        CHECK((pixels[24 * stride + width - 136] & 0xffffff) == 0);

        const char *directory = getenv("NATIVE_RENDER_OUTPUT");

        if (directory) {
            char path[1024];
            static const char *names[] = {
                "native-library-fixture.png",         "native-login-render.png",
                "native-compact-login.png",           "native-home-fixture.png",
                "native-devices-fixture.png",         "native-compact-library-fixture.png",
                "native-setup-fixture.png",           "native-search-fixture.png",
                "native-search-scrolled-fixture.png", "native-home-wide-fixture.png",
                "native-home-compact-fixture.png",    "native-home-bottom-fixture.png",
                "native-now-playing-fixture.png",     "native-friends-panel-fixture.png",
                "native-lyrics-panel-fixture.png",    "native-queue-panel-fixture.png",
                "native-album-detail-fixture.png",    "native-artist-detail-fixture.png",
                "native-profile-detail-fixture.png"};
            snprintf(path, sizeof(path), "%s/%s", directory, names[state]);
            CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
        }

        cairo_surface_destroy(surface);
    }

    linux_canvas_destroy(&backend);
    test_navigation_rendering();
    pango_cairo_font_map_set_default(NULL);
    cairo_debug_reset_static_data();
    FcConfigAppFontClear(FcConfigGetCurrent());
    FcFini();
}
#endif

#ifdef _WIN32
static void test_native_text_compositor(void) {
    WindowsCanvas backends[2] = {0};
    Canvas canvases[2];
    HDC paints[2];
    HBITMAP images[2];
    HGDIOBJ originals[2];
    uint32_t *pixels[2];

    for (unsigned i = 0; i < 2; ++i) {
        unsigned scale = i == 0 ? 2 : 1;
        paints[i] = CreateCompatibleDC(NULL);
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = (LONG)(256 * scale);
        info.bmiHeader.biHeight = -(LONG)(128 * scale);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *data = NULL;
        images[i] = CreateDIBSection(paints[i], &info, DIB_RGB_COLORS, &data, NULL, 0);
        CHECK(paints[i] && images[i] && data);
        pixels[i] = data;
        originals[i] = SelectObject(paints[i], images[i]);
        windows_canvas_begin(&backends[i], paints[i], (float)scale, &canvases[i]);
    }

    windows_canvas_defer_text(&backends[0], &backends[1]);
    canvases[1].box(canvases[1].context, (Rect){0, 0, 256, 128}, 0, 0x552233);
    canvases[0].box(canvases[0].context, (Rect){0, 0, 256, 128}, 0, 0);
    canvases[0].text(canvases[0].context, "Hidden behind modal", (Rect){20, 20, 210, 40}, 14, 0, 0xffffff);
    canvases[0].box(canvases[0].context, (Rect){0, 0, 256, 70}, 0, 0);
    canvases[0].box(canvases[0].context, (Rect){20, 76, 40, 20}, 10, 0xffffff);
    canvases[0].ellipse(canvases[0].context, (Rect){80, 76, 20, 20}, true, 1, 0xffffff);
    canvases[0].text(canvases[0].context, "gypq", (Rect){20, 82, 210, 4}, 14, 0, 0x00ff00);
    GdiFlush();
    CHECK((pixels[1][30 * 256 + 30] & 0xffffff) == 0x552233);
    CHECK(windows_canvas_present(&backends[0], paints[1], 256, 128));
    GdiFlush();
    unsigned visible = 0;
    unsigned partial = 0;

    for (unsigned y = 0; y < 128; ++y) {
        for (unsigned x = 0; x < 256; ++x) {
            if (y < 70) {
                CHECK((pixels[1][y * 256 + x] & 0xffffff) == 0);
            }

            if (y >= 74 && y <= 98 && x >= 18 && x <= 104) {
                uint32_t red = (pixels[1][y * 256 + x] >> 16) & 255;
                if (red > 8 && red < 248) {
                    ++partial;
                }
            }

            if (y > 86 && (pixels[1][y * 256 + x] & 0x00ff00)) {
                ++visible;
            }
        }
    }

    CHECK(visible > 0 && partial > 0);

    HBITMAP frame = backends[0].frame_image;

    for (unsigned i = 0; i < 4; ++i) {
        windows_canvas_defer_text(&backends[0], &backends[1]);
        canvases[0].box(canvases[0].context, (Rect){0, 0, 256, 128}, 0, 0x102030);
        canvases[0].text(canvases[0].context, "Hover repaint", (Rect){20, 20, 210, 30}, 14, 0, 0xffffff);
        GdiFlush();
        CHECK((pixels[1][0] & 0xffffff) == (i ? 0x102030 : 0));
        CHECK(windows_canvas_present(&backends[0], paints[1], 256, 128));
        GdiFlush();
        CHECK((pixels[1][0] & 0xffffff) == 0x102030);
        CHECK(backends[0].frame_image == frame);
        CHECK(backends[0].text_target == NULL);
    }

    windows_canvas_defer_text(&backends[0], &backends[1]);
    canvases[0].text(canvases[0].context, "Discarded frame", (Rect){20, 20, 210, 30}, 14, 0, 0xffffff);
    CHECK(!windows_canvas_present(&backends[0], paints[1], 0, 128));
    CHECK(backends[0].text_target == NULL);
    CHECK(windows_canvas_present(&backends[0], paints[1], 128, 64));
    CHECK(backends[0].frame_width == 128 && backends[0].frame_height == 64);

    for (unsigned i = 0; i < 2; ++i) {
        windows_canvas_destroy(&backends[i]);
        SelectObject(paints[i], originals[i]);
        DeleteObject(images[i]);
        DeleteDC(paints[i]);
    }
}
#endif

int main(void) {
    CHECK(platform_init());
    app_initialize();
    test_no_session();
    test_layout();
    test_input_and_activation();
    test_animation();
    test_content_geometry();
    test_player_drag();
    test_login_screen();
    test_search_and_library_geometry();
    test_collection_clicks_do_not_play();
    test_home_scrolling_and_panels();
    test_label_alignment();

#ifndef _WIN32
    test_rendering();
#else
    test_native_text_compositor();
#endif

    printf("PASS: %u interaction checks (native scene layout, hit testing, input, animation settling, "
           "deferred commands)\n",
           checks);
    return 0;
}
