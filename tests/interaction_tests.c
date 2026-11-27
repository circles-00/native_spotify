#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "app_internal.h"
#include "platform.h"
#include "player_fixture.h"
#include "ui/canvas_raster.h"
#include "ui/controller.h"
#include "ui/dark_palette.h"
#include "ui/scene.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include "platform/linux_canvas.h"
#include <fontconfig/fontconfig.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include "platform/win32_canvas.h"
#else
#include <time.h>
#include <unistd.h>
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
    View *snapshot = malloc(sizeof(*snapshot));
    CHECK(snapshot != NULL);

    for (unsigned i = 0; i < 200; ++i) {
        app_snapshot_into(snapshot);

        if (!snapshot->list.loading) {
            break;
        }

#ifdef _WIN32
        Sleep(5);
#else
        struct timespec interval = {0, 5000000};
        nanosleep(&interval, NULL);
#endif
    }

    app_snapshot_into(snapshot);
    CHECK(!snapshot->list.loading);
    free(snapshot);
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
    View *snapshot = malloc(sizeof(*snapshot));
    CHECK(snapshot != NULL);
    app_snapshot_into(snapshot);
    CHECK(snapshot->engine == ENGINE_OFF && !snapshot->list.loaded && !snapshot->list.loading);
    CHECK(!app_browse(BROWSE_SEARCH, ""));
    CHECK(!app_select(0));
    CHECK(!app_activate());
    CHECK(!app_open_link());
    CHECK(!app_transfer());
    CHECK(!app_seek(500));
    CHECK(!app_volume(50));
    CHECK(!app_select_device(0));
    CHECK(!app_load_more());
    CHECK(!list_row(&snapshot->list, 0));

    // Commands are refused before any optimistic change when there is no session.
    app_snapshot_into(snapshot);
    unsigned revision = snapshot->playback_revision;
    app_action(ACTION_PLAY);
    app_snapshot_into(snapshot);
    CHECK(!snapshot->playback.playing && snapshot->playback_revision == revision);
    CHECK(!strcmp(snapshot->status, "Connect your Spotify account first."));

    app_action(ACTION_DEVICES);
    app_snapshot_into(snapshot);
    CHECK(!snapshot->devices.count);

    app_action(ACTION_QUEUE);
    wait_loaded();
    app_snapshot_into(snapshot);
    CHECK(!snapshot->queue.count);
    CHECK(!strcmp(snapshot->status, "Connect your Spotify account first."));

    // Invalid Soloist keys are rejected immediately and never reach the credential store.
    CHECK(!app_engine_key("short", 5));
    app_snapshot_into(snapshot);
    CHECK(strstr(snapshot->engine_message, "does not contain a Soloist key") != NULL);
    free(snapshot);
}

static void test_layout(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;

    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.x == 0 && scene.sidebar.y == 0 && scene.sidebar.w == 223);
    CHECK(scene.content.x == scene.sidebar.x + scene.sidebar.w);
    CHECK(scene_view_scale(1920, 1200, 1.0f) == 1.5f && scene_view_scale(1920, 800, 1.0f) == 1.0f);
    CHECK(scene.player.y >= scene.content.y + scene.content.h);
    CHECK(scene.search.x > scene.content.x && scene.search.y + scene.search.h < scene.content.y);
    CHECK(scene.search.w > 320 && scene.search.h > 30);
    float content_center = scene.content.x + scene.content.w * 0.5f;
    float search_center = scene.search.x + scene.search.w * 0.5f;
    CHECK(search_center >= content_center - 1.0f && search_center <= content_center + 1.0f);

    Rect back = scene.hits[HIT_BACK].rect;
    Rect forward = scene.hits[HIT_FORWARD].rect;
    Rect top_home = scene.hits[HIT_TOP_HOME].rect;
    CHECK(back.x == scene.sidebar.w + 21 && back.y == 11 && back.w == 28 && back.h == 28);
    CHECK(forward.x == scene.sidebar.w + 57 && forward.y == 11 && forward.w == 28 && forward.h == 28);
    CHECK(scene.hits[HIT_TOP_HOME].enabled);
    CHECK(top_home.x > forward.x + forward.w && top_home.x + top_home.w < scene.search.x);
    CHECK(scene_hit(&scene, top_home.x + top_home.w * 0.5f, top_home.y + top_home.h * 0.5f) == HIT_TOP_HOME);

    CHECK(scene_hit(&scene, scene.search.x + 60, 24) == HIT_SEARCH);
    CHECK(scene_hit(&scene, 0, 0) == HIT_NONE);
    Rect nav = scene.hits[HIT_SEARCH_NAV].rect;
    CHECK(scene_hit(&scene, nav.x + 12, nav.y + 12) == HIT_SEARCH_NAV);
    scene_activate(&scene, &view, HIT_SEARCH_NAV);
    CHECK(scene.focus == HIT_SEARCH && scene.select_all);
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

    // Nothing is focused at start, so typing goes nowhere until the search box is picked.
    CHECK(scene.focus == HIT_NONE && !scene_input(&scene, "x", 1));
    scene.focus = HIT_SEARCH;

    CHECK(scene_input(&scene, "Björk ♫", strlen("Björk ♫")));
    CHECK(scene_key(&scene, &view, KEY_BACKSPACE, false));
    CHECK(!strcmp(scene.query, "Björk "));
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_SEARCH_CLEAR].enabled);
    Rect clear = scene.hits[HIT_SEARCH_CLEAR].rect;
    CHECK(scene_hit(&scene, clear.x + clear.w / 2, clear.y + clear.h / 2) == HIT_SEARCH_CLEAR);
    scene_activate(&scene, &view, HIT_SEARCH_CLEAR);
    CHECK(!scene.query[0] && scene.focus == HIT_SEARCH);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.hits[HIT_SEARCH_CLEAR].enabled);

    CHECK(scene_input(&scene, "Björk ", strlen("Björk ")));
    scene.select_all = true;
    CHECK(scene_input(&scene, "Jóga", strlen("Jóga")));
    CHECK(!strcmp(scene.query, "Jóga"));
    CHECK(scene_key(&scene, &view, KEY_ENTER, false));
    wait_loaded();
    CHECK(!scene.home);
    {
        View *status_view = malloc(sizeof(*status_view));
        CHECK(status_view != NULL);
        app_snapshot_into(status_view);
        CHECK(!strcmp(status_view->status, "Connect your Spotify account first."));
        free(status_view);
    }

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

    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_BACK].enabled && !scene.hits[HIT_FORWARD].enabled);
    scene_activate(&scene, &view, HIT_BACK);
    wait_loaded();
    CHECK(!scene.home && scene.section == BROWSE_SEARCH && !strcmp(scene.query, "Jóga"));
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_FORWARD].enabled);
    scene_activate(&scene, &view, HIT_FORWARD);
    wait_loaded();
    CHECK(!scene.home && scene.section == BROWSE_SAVED_ALBUMS);

    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false));
    CHECK(scene.focus == HIT_NONE);

    ++view.session_revision;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.query[0] && scene.home && scene.panel == PANEL_NONE);
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
    CHECK(scene.first_row <= 12 && scene.first_row % scene.main_columns == 0);
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

    // The speaker button opens the devices sidebar. Without a ready Soloist, "This computer" opens setup.
    scene_activate(&scene, &view, HIT_DEVICES);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_DEVICES);
    Rect computer = scene.hits[HIT_TRANSFER].rect;
    CHECK(scene.hits[HIT_TRANSFER].enabled && computer.x >= scene.side_panel.x);
    CHECK(scene_hit(&scene, computer.x + 10, computer.y + 10) == HIT_TRANSFER);
    scene_activate(&scene, &view, HIT_TRANSFER);
    CHECK(scene.setup_popup);
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

    // With Soloist ready, the setup row disappears and other devices are listed and scrollable.
    view.engine = ENGINE_READY;
    scene.panel = PANEL_NONE;
    scene_activate(&scene, &view, HIT_DEVICES);
    CHECK(devices_parse(DEVICES_FIXTURE, &view.devices));
    view.devices.count = DEVICE_LIMIT;
    ++view.devices_revision;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.hits[HIT_TRANSFER].enabled);
    CHECK(scene.device_visible > 0 && scene.device_visible < DEVICE_LIMIT);
    CHECK(!scene.hits[HIT_DEVICE_LAST].rect.w);
    scene.over_panel = true;

    for (unsigned i = 0; i < 40; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_DEVICE_LAST].rect.w > 0);
    CHECK(scene.hits[HIT_DEVICE_LAST].rect.y + scene.hits[HIT_DEVICE_LAST].rect.h <=
          scene.side_panel.y + scene.side_panel.h);
    scene.over_panel = false;
    view.engine = ENGINE_OFF;
    view.devices.count = 0;
    scene_activate(&scene, &view, HIT_PANEL_CLOSE);
    CHECK(scene.panel == PANEL_NONE);

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
    view.settings.client_id[0] = 0;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.login_screen && scene.focus == HIT_CLIENT_ID);
    CHECK(scene.hits[HIT_CLIENT_ID].enabled && scene.hits[HIT_ACCOUNT].enabled);
    CHECK(scene.hits[HIT_DEV_DASHBOARD].enabled && scene.hits[HIT_DEV_GUIDE].enabled);
    CHECK(!scene.hits[HIT_ROW_FIRST].enabled && !scene.hits[HIT_CARD_FIRST].enabled);
    CHECK(scene.login_panel.x + scene.login_panel.w / 2 == 640);
    CHECK(scene.login_panel.y + scene.login_panel.h / 2 == 400);
    CHECK(scene.hits[HIT_ACCOUNT].rect.x + scene.hits[HIT_ACCOUNT].rect.w / 2 == 640);
    CHECK(!scene.hits[HIT_SEARCH].enabled && !scene.hits[HIT_VOLUME].enabled);
    CHECK(client_id_valid("0123456789abcdef0123456789abcdef"));
    CHECK(!client_id_valid(""));
    CHECK(app_tick(false) == -1 && app_tick(true) == -1);

    Scene saved_login;
    scene_init(&saved_login);
    snprintf(view.settings.client_id, sizeof(view.settings.client_id), "%s",
             "0123456789abcdef0123456789abcdef");
    scene_layout(&saved_login, &view, 1280, 800);
    CHECK(saved_login.login_screen && saved_login.focus == HIT_ACCOUNT);
    CHECK(!strcmp(saved_login.client_id, "0123456789abcdef0123456789abcdef"));

    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.login_panel.x + scene.login_panel.w / 2 == 480);
    CHECK(scene.login_panel.y + scene.login_panel.h / 2 == 320);
    view.connecting = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.hits[HIT_ACCOUNT].enabled && !scene.hits[HIT_CLIENT_ID].enabled);
    view.connecting = false;
    view.authenticated = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(!scene.login_screen && scene.focus == HIT_NONE);

    view.library[1].count = 1;
    view.library[2].count = 2;
    view.daily_mix_count = 1;
    view.suggested_playlist_count = 1;
    view.recommended_track_count = 1;
    CHECK(scene_home_item(&view, 0) == &view.library[2].items[0]);
    CHECK(scene_home_item(&view, 1) == &view.library[2].items[1]);
    CHECK(scene_home_item(&view, HOME_SHELF_COLUMNS) == NULL);
    CHECK(scene_home_item(&view, 2 * HOME_SHELF_COLUMNS) == &view.daily_mixes[0]);
    CHECK(scene_home_item(&view, 3 * HOME_SHELF_COLUMNS) == &view.suggested_playlists[0]);
    CHECK(scene_home_item(&view, 4 * HOME_SHELF_COLUMNS) == &view.recommended_tracks[0]);
    CHECK(scene_home_item(&view, 5 * HOME_SHELF_COLUMNS) == &view.library[1].items[0]);
    CHECK(scene_home_quick_item(&view, 0) == NULL);
    CHECK(scene_home_quick_item(&view, 1) == &view.library[2].items[0]);
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
    view.search_playlist_count = 5;
    view.playlists.count = 100;
    view.playlists.rows_count = LIST_WINDOW;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_SEARCH_ALL].enabled && scene.hits[HIT_SEARCH_ALBUMS].enabled &&
          scene.hits[HIT_SEARCH_PLAYLISTS].enabled);
    CHECK(scene.hits[HIT_SEARCH_TOP].rect.x < scene.hits[HIT_ROW_FIRST].rect.x);
    CHECK(scene.hits[HIT_ROW_FIRST + 3].enabled && !scene.hits[HIT_ROW_FIRST + 4].enabled);
    CHECK(scene.hits[HIT_SEARCH_ARTIST_FIRST].enabled && scene.hits[HIT_SEARCH_ALBUM_FIRST].enabled &&
          scene.hits[HIT_SEARCH_PLAYLIST_FIRST].enabled);
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

    unsigned end = scene.library_first;
    CHECK(end > 80 && end < view.playlists.count);
    scene_scroll(&scene, &view, -1);
    CHECK(scene.library_first < end);
    unsigned first = scene.library_first;
    scene.setup_popup = true;
    scene_scroll(&scene, &view, -1);
    CHECK(scene.library_first == first);
    app_viewport(0);
    app_library_viewport(0);
}

static void test_collection_clicks_do_not_play(void) {
    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);

    CatalogItem artist = {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijkl", .title = "Fixture artist"};
    app_snapshot_into(view);
    unsigned revision = view->playback_revision;
    CHECK(app_open_item(&artist));
    wait_loaded();
    app_snapshot_into(view);
    CHECK(view->list.request.kind == BROWSE_ARTIST_ALBUMS);
    CHECK(view->playback_revision == revision);

    Scene scene;
    scene_init(&scene);
    app_snapshot_into(view);
    view->authenticated = true;
    view->search_artists[0] = artist;
    view->search_artist_count = 1;
    scene.home = false;
    scene.section = BROWSE_SEARCH;
    scene_layout(&scene, view, 1280, 800);
    scene_activate(&scene, view, HIT_SEARCH_TOP);
    wait_loaded();
    CHECK(scene.section == BROWSE_ARTIST_ALBUMS && !scene.home);
    CHECK(!strcmp(scene.collection, "Fixture artist"));
    app_snapshot_into(view);
    CHECK(view->playback_revision == revision);

    CatalogItem album = {.kind = ITEM_ALBUM, .id = "0123456789abcdefghijkl", .title = "Fixture album"};
    CHECK(app_open_item(&album));
    wait_loaded();
    app_snapshot_into(view);
    CHECK(view->list.request.kind == BROWSE_ALBUM_TRACKS);
    CHECK(view->playback_revision == revision);

    /* Context-aware top result picks matching track or playlist, and search playlists open properly. */
    CatalogItem playlist = {.kind = ITEM_PLAYLIST,
                            .id = "0123456789abcdefghijkl",
                            .title = "lofi beats",
                            .detail = "Spotify",
                            .owner_id = "spotify"};
    CatalogItem song = {.kind = ITEM_TRACK,
                        .id = "0123456789abcdefghijk2",
                        .title = "Bohemian Rhapsody - Remastered 2011",
                        .detail = "Queen",
                        .playable = true};
    app_snapshot_into(view);
    view->authenticated = true;
    view->list.loaded = true;
    view->list.request.kind = BROWSE_SEARCH;
    copy_utf8(view->list.request.query, sizeof(view->list.request.query), "Bohemian Rhapsody");
    view->list.count = 1;
    view->list.rows_count = 1;
    view->list.rows[0] = song;
    view->search_artists[0] =
        (CatalogItem){.kind = ITEM_ARTIST, .id = "0123456789abcdefghijk3", .title = "Queen"};
    view->search_artist_count = 1;
    view->search_playlists[0] = playlist;
    view->search_playlist_count = 1;
    scene_init(&scene);
    scene.home = false;
    scene.section = BROWSE_SEARCH;
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.hits[HIT_TOP_LIKE].enabled);

    copy_utf8(view->list.request.query, sizeof(view->list.request.query), "lofi beats");
    scene_layout(&scene, view, 1280, 800);
    CHECK(!scene.hits[HIT_TOP_LIKE].enabled);
    scene_activate(&scene, view, HIT_SEARCH_TOP);
    wait_loaded();
    CHECK(scene.section == BROWSE_PLAYLIST_TRACKS && !strcmp(scene.collection, "lofi beats"));

    scene.home = false;
    scene.section = BROWSE_SEARCH;
    copy_utf8(scene.query, sizeof(scene.query), "lofi beats");
    Rect chip = scene.hits[HIT_SEARCH_PLAYLISTS].rect;
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene_hit(&scene, chip.x + 10, chip.y + 10) == HIT_SEARCH_PLAYLISTS);
    scene_activate(&scene, view, HIT_SEARCH_PLAYLISTS);
    wait_loaded();
    CHECK(scene.search_filter == SEARCH_PLAYLISTS && scene.section == BROWSE_SEARCH);
    free(view);
}

static void test_song_radio_and_art_play(void) {
    View view = app_snapshot();
    unsigned playback_revision = view.playback_revision;
    unsigned setup_revision = view.setup_revision;
    CatalogItem seed = {.kind = ITEM_TRACK,
                        .id = "0123456789abcdefghijk2",
                        .title = "Seed song",
                        .playable = true};
    view.authenticated = true;
    view.list = (ListView){.request = {.kind = BROWSE_SAVED_TRACKS}, .loaded = true,
                           .selected = -1, .count = 1, .total = 1, .rows_count = 1};
    view.list.rows[0] = seed;

    Scene scene;
    scene_init(&scene);
    scene.session_revision = view.session_revision;
    scene.home = false;
    scene.section = BROWSE_SAVED_TRACKS;
    scene_layout(&scene, &view, 1280, 800);
    Rect art = scene_song_art(&scene, HIT_ROW_FIRST);
    float x = art.x + art.w / 2;
    float y = art.y + art.h / 2;
    CHECK(scene_hit(&scene, x, y) == HIT_ROW_FIRST);
    scene_context_press(&scene, &view, x, y);
    CHECK(scene.menu.open);

    unsigned choice = 0;
    while (choice < scene.menu.count && scene.menu.actions[choice] != CTX_RADIO) {
        ++choice;
    }

    CHECK(choice < scene.menu.count);
    scene_activate(&scene, &view, (HitId)(HIT_CTX_FIRST + choice));
    wait_loaded();
    CHECK(scene.section == BROWSE_SONG_RADIO && !scene.home);
    app_snapshot_into(&view);
    CHECK(view.playback_revision == playback_revision && view.setup_revision == setup_revision);
    CHECK(view.list.request.kind == BROWSE_SONG_RADIO);

    /* Run the content job without credentials to exercise the seed-only fallback. */
    platform_lock();
    Job radio = {.action = ACTION_BROWSE, .browse = view.list.request, .item = app_state.radio_seed,
                 .session = view.session_revision, .browse_seq = app_state.browse_seq};
    platform_unlock();
    Spotify offline = {0};
    app_run_content(&offline, &radio);
    app_snapshot_into(&view);
    CHECK(view.list.loaded && view.list.request.kind == BROWSE_SONG_RADIO);
    CHECK(view.list.count == 1 && view.list.total == 1 && !view.list.more);
    CHECK(!strcmp(view.list.request.title, "Seed song Radio"));
    const CatalogItem *first = list_row(&view.list, 0);
    CHECK(first && !strcmp(first->id, seed.id));

    view.authenticated = true;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_OPEN].enabled && view.list.selected == -1);
    art = scene_song_art(&scene, HIT_ROW_FIRST);
    scene_press(&scene, &view, art.x + art.w / 2, art.y + art.h / 2, 1000);
    CHECK(scene.pressed_song_art && !scene.double_click);
    scene_release(&scene, &view, art.x + art.w / 2, art.y + art.h / 2);
    app_snapshot_into(&view);
    CHECK(view.list.selected == 0 && view.setup_revision == setup_revision + 1);

    platform_lock();
    app_state.view.setup_revision = setup_revision;
    EngineState previous_engine = app_state.view.engine;
    app_state.view.engine = ENGINE_STARTING;
    platform_unlock();

    SongRadio *fixture = malloc(sizeof(*fixture));
    CHECK(fixture != NULL);
    fixture->tracks[0] = seed;

    for (unsigned i = 1; i < SONG_RADIO_LIMIT; ++i) {
        fixture->tracks[i] = seed;
        snprintf(fixture->tracks[i].id, sizeof(fixture->tracks[i].id), "%022u", i);
    }

    for (unsigned count = 49; count <= 50; ++count) {
        fixture->count = count;
        platform_lock();
        bool stored = app_collection_store_radio(&view.list.request, fixture);
        platform_unlock();
        CHECK(stored);
        app_snapshot_into(&view);
        CHECK(view.list.count == count && view.list.total == count && !view.list.more);
        CHECK(app_play_song_radio());
        platform_lock();
        Job playback = app_state.deferred;
        platform_unlock();
        CHECK(playback.track_count == count);
        CHECK(!strcmp(playback.track_ids[0], seed.id));
        CHECK(!strcmp(playback.track_ids[count - 1], fixture->tracks[count - 1].id));
    }

    free(fixture);
    platform_lock();
    app_state.has_deferred = false;
    app_state.view.engine = previous_engine;
    platform_unlock();
}

static void test_like_status_before_scan(void) {
    CatalogItem song = {.kind = ITEM_TRACK, .id = "0000000000000000000021", .title = "Liked", .playable = true};
    Worker *worker = &app_state.workers[LANE_STATE];

    for (unsigned i = 0; i < 200; ++i) {
        platform_lock();
        bool idle = !worker->running;
        platform_unlock();

        if (idle) {
            break;
        }

        platform_sleep(5);
    }

    platform_lock();
    CHECK(!worker->running);
    app_state.view.authenticated = true;
    app_state.likes_ready = false;
    app_state.view.queue.count = 1;
    app_state.view.queue.items[0] = song;
    worker->running = true;
    unsigned queued = worker->count;
    platform_unlock();

    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);
    app_snapshot_into(view);
    CHECK(!view->queue.items[0].liked);

    platform_lock();
    CHECK(worker->count == queued + 1);
    Job job = worker->jobs[(worker->head + worker->count - 1) % LANE_CAPACITY];
    worker->count = queued;
    platform_unlock();
    CHECK(job.action == ACTION_LIKE_STATUS && job.track_count == 1 && !strcmp(job.track_ids[0], song.id));

    // A second frame must not ask again while the answer is pending.
    app_snapshot_into(view);
    platform_lock();
    CHECK(worker->count == queued);
    platform_unlock();

    bool liked[] = {true};
    app_finish_like_status(&job, liked);
    app_snapshot_into(view);
    CHECK(view->queue.items[0].liked);

    platform_lock();
    worker->running = false;
    app_state.view.queue.count = 0;
    platform_unlock();
    free(view);
    app_reset_session();
}

static void test_playlist_sort_filter_and_play(void) {
    BrowseRequest request = {.kind = BROWSE_PLAYLIST_TRACKS, .id = "0000000000000000000031", .title = "Mix"};
    BrowsePage page = {.count = 3, .limit = 3, .total = 3};
    const char *titles[] = {"Charlie", "alpha", "Bravo"};
    const unsigned durations[] = {200000, 300000, 100000};

    for (unsigned i = 0; i < 3; ++i) {
        page.items[i] = (CatalogItem){.kind = ITEM_TRACK, .playable = true, .duration_ms = durations[i]};
        snprintf(page.items[i].id, sizeof(page.items[i].id), "00000000000000000000%02u", 40 + i);
        copy_utf8(page.items[i].title, sizeof(page.items[i].title), titles[i]);
    }

    platform_lock();
    app_state.view.authenticated = true;
    app_state.view.list = (ListView){.request = request, .selected = -1};
    CHECK(app_collection_store(&request, &page, false));
    EngineState previous_engine = app_state.view.engine;
    app_state.view.engine = ENGINE_STARTING;
    platform_unlock();

    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);
    Scene scene;
    scene_init(&scene);
    scene.session_revision = app_state.view.session_revision;
    scene.home = false;
    scene.section = BROWSE_PLAYLIST_TRACKS;
    app_snapshot_into(view);
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.hits[HIT_OPEN].enabled && view->list.selected == -1);
    CHECK(scene.hits[HIT_LIST_FILTER].enabled && scene.hits[HIT_SORT_TITLE].enabled);

    scene_activate(&scene, view, HIT_SORT_TITLE);
    app_snapshot_into(view);
    CHECK(view->list.sort == LIST_SORT_TITLE && !view->list.sort_descending);
    CHECK(!strcmp(list_row(&view->list, 0)->title, "alpha") && !strcmp(list_row(&view->list, 2)->title, "Charlie"));

    scene_activate(&scene, view, HIT_SORT_TITLE);
    app_snapshot_into(view);
    CHECK(view->list.sort_descending && !strcmp(list_row(&view->list, 0)->title, "Charlie"));

    // Playing a sorted playlist follows the order on screen.
    scene_layout(&scene, view, 1280, 800);
    scene_activate(&scene, view, HIT_OPEN);
    platform_lock();
    Job playback = app_state.deferred;
    platform_unlock();
    CHECK(playback.track_count == 3 && !strcmp(playback.track_ids[0], page.items[0].id));

    scene_activate(&scene, view, HIT_SORT_TITLE);
    app_snapshot_into(view);
    CHECK(view->list.sort == LIST_SORT_NONE && !strcmp(list_row(&view->list, 0)->title, "Charlie"));

    scene.focus = HIT_LIST_FILTER;
    CHECK(scene_input(&scene, "BRA", 3));
    app_snapshot_into(view);
    CHECK(view->list.count == 1 && !strcmp(list_row(&view->list, 0)->title, "Bravo"));
    CHECK(scene_key(&scene, view, KEY_BACKSPACE, false));
    app_snapshot_into(view);
    CHECK(!strcmp(view->list.filter, "BR"));

    scene_layout(&scene, view, 1280, 800);
    scene_activate(&scene, view, HIT_LIST_FILTER_CLEAR);
    app_snapshot_into(view);
    CHECK(view->list.count == 3 && !view->list.filter[0]);

    platform_lock();
    app_state.has_deferred = false;
    app_state.view.engine = previous_engine;
    platform_unlock();
    free(view);
    app_reset_session();
}

static void test_devices_panel_and_badge(void) {
    Worker *worker = &app_state.workers[LANE_PLAYER];
    platform_lock();
    app_state.view.authenticated = true;
    app_state.view.engine = ENGINE_READY;
    copy_utf8(app_state.view.engine_device_id, sizeof(app_state.view.engine_device_id), "local");
    app_state.view.devices.count = 2;
    app_state.view.devices.items[0] = (Device){.id = "local", .name = "Soloist", .type = "Computer"};
    app_state.view.devices.items[1] =
        (Device){.id = "kitchen", .name = "Kitchen", .type = "Speaker", .active = true, .supports_volume = true};
    app_state.view.playback = (Playback){.has_device = true, .playing = true, .device = "Kitchen",
                                         .device_id = "kitchen", .volume_percent = 40,
                                         .supports_volume = true};
    ++app_state.view.playback_revision;
    worker->running = true;
    unsigned queued = worker->count;
    platform_unlock();

    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);
    app_snapshot_into(view);
    Scene scene;
    scene_init(&scene);
    scene.session_revision = view->session_revision;
    scene.caption_width = 120;
    scene_layout(&scene, view, 1280, 800);

    // Another device is playing, so the header shows the badge without overlapping search or profile.
    Rect badge = scene.hits[HIT_PLAYING_ON].rect;
    Rect profile = scene.hits[HIT_PROFILE].rect;
    CHECK(scene.hits[HIT_PLAYING_ON].enabled);
    CHECK(badge.x >= scene.search.x + scene.search.w + 8 && badge.x + badge.w <= profile.x);
    CHECK(badge.y + badge.h / 2 == profile.y + profile.h / 2);
    CHECK(profile.y + profile.h / 2 == scene.search.y + scene.search.h / 2);
    CHECK(profile.x + profile.w < scene.header.w - scene.caption_width);
    CHECK(scene_hit(&scene, badge.x + 10, badge.y + 10) == HIT_PLAYING_ON);
    scene_activate(&scene, view, HIT_PLAYING_ON);
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.panel == PANEL_DEVICES);
    CHECK(scene.side_panel.y == scene.header.h && scene.side_panel.y + scene.side_panel.h == scene.player.y);
    CHECK(scene.hits[HIT_PROFILE].rect.x == profile.x);
    CHECK(scene_hit(&scene, profile.x + profile.w / 2, profile.y + profile.h / 2) == HIT_PROFILE);

    // The playing device sits in the card; only this computer is offered below, and one click moves playback.
    CHECK(scene.hits[HIT_DEVICE_FIRST].enabled && !scene.hits[HIT_DEVICE_FIRST + 1].rect.w);
    CHECK(scene.hits[HIT_PANEL_PLAY].enabled);
    scene_activate(&scene, view, HIT_DEVICE_FIRST);
    platform_lock();
    CHECK(worker->count == queued + 1);
    Job transfer = worker->jobs[(worker->head + worker->count - 1) % LANE_CAPACITY];
    worker->count = queued;
    platform_unlock();
    CHECK(transfer.action == ACTION_TRANSFER && !strcmp(transfer.device.id, "local"));

    // The volume icon mutes and restores the previous level.
    CHECK(scene.hits[HIT_MUTE].enabled && scene.draft.volume == 40);
    scene_activate(&scene, view, HIT_MUTE);
    CHECK(scene.draft.volume == 0);
    scene_activate(&scene, view, HIT_MUTE);
    CHECK(scene.draft.volume == 40);

    // Playing on this computer hides the badge.
    platform_lock();
    worker->count = queued;
    copy_utf8(app_state.view.playback.device_id, sizeof(app_state.view.playback.device_id), "local");
    worker->running = false;
    platform_unlock();
    app_snapshot_into(view);
    scene_layout(&scene, view, 1280, 800);
    CHECK(!scene.hits[HIT_PLAYING_ON].enabled);

    free(view);
    app_reset_session();
}

static CatalogItem queue_song(unsigned number) {
    CatalogItem song = {.kind = ITEM_TRACK, .playable = true, .duration_ms = 180000, .duration_known = true};
    snprintf(song.id, sizeof(song.id), "00000000000000000000%02u", number);
    snprintf(song.title, sizeof(song.title), "Song %u", number);
    return song;
}

static void test_app_managed_queue(void) {
    Spotify offline = {0};
    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);

    platform_lock();
    app_state.view.authenticated = true;
    app_state.view.playback = (Playback){.has_device = true, .playing = true, .device_id = "phone",
                                         .uri = "spotify:track:0000000000000000000090"};
    app_state.view.queue = (Queue){.count = 2};
    app_state.view.queue.items[0] = queue_song(80);
    app_state.view.queue.items[1] = queue_song(81);
    platform_unlock();

    // Added songs stay in the app and are shown ahead of Spotify's own upcoming songs.
    CatalogItem first = queue_song(1);
    CatalogItem second = queue_song(2);
    CatalogItem third = queue_song(3);
    CHECK(app_enqueue_item(&first) && app_enqueue_item(&second) && app_enqueue_item(&third));
    app_snapshot_into(view);
    CHECK(view->queue.queued == 3 && view->queue.count == 5);
    CHECK(!strcmp(view->queue.items[0].id, first.id) && !strcmp(view->queue.items[3].id, "0000000000000000000080"));

    // Spotify is handed the first song; it is shown once and leaves the app queue when it starts.
    platform_lock();
    copy_utf8(app_state.handoff_id, sizeof(app_state.handoff_id), first.id);
    app_state.view.queue.items[0] = first;
    platform_unlock();
    app_snapshot_into(view);
    CHECK(view->queue.queued == 3 && view->queue.count == 4);

    Playback now = {.has_device = true, .device_id = "phone", .uri = "spotify:track:0000000000000000000001"};
    app_up_next_sync(&offline, &now, "spotify:track:0000000000000000000090", view->session_revision);
    app_snapshot_into(view);
    CHECK(view->queue.queued == 2 && !strcmp(view->queue.items[0].id, second.id));
    platform_lock();
    CHECK(!app_state.handoff_id[0]);
    platform_unlock();

    // Removing a song and clearing the queue work on the app's list.
    CHECK(app_dequeue(third.id));
    app_snapshot_into(view);
    CHECK(view->queue.queued == 1);

    Scene scene;
    scene_init(&scene);
    scene.session_revision = view->session_revision;
    scene.home = false;
    scene.section = BROWSE_SAVED_TRACKS;
    scene_activate(&scene, view, HIT_QUEUE);
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.panel == PANEL_QUEUE && scene.hits[HIT_QUEUE_CLEAR].enabled);
    Rect row = scene.hits[HIT_QUEUE_ROW_FIRST].rect;
    scene_context_press(&scene, view, row.x + row.w / 2, row.y + row.h / 2);
    CHECK(scene.menu.open);
    bool remove = false;

    for (unsigned i = 0; i < scene.menu.count; ++i) {
        remove = remove || scene.menu.actions[i] == CTX_DEQUEUE;
    }

    CHECK(remove);
    scene.menu.open = false;

    // A song handed to Spotify but then cleared is skipped once when Spotify reaches it.
    platform_lock();
    copy_utf8(app_state.handoff_id, sizeof(app_state.handoff_id), second.id);
    platform_unlock();
    scene_activate(&scene, view, HIT_QUEUE_CLEAR);
    app_snapshot_into(view);
    CHECK(view->queue.queued == 0);
    platform_lock();
    CHECK(!app_state.handoff_id[0] && !strcmp(app_state.stale_handoffs[0].id, second.id));
    app_state.view.queue.items[0] = second;
    platform_unlock();
    app_snapshot_into(view);
    CHECK(strcmp(view->queue.items[0].id, second.id) != 0);

    now = (Playback){.has_device = true, .device_id = "phone", .uri = "spotify:track:0000000000000000000002"};
    app_up_next_sync(&offline, &now, "spotify:track:0000000000000000000001", view->session_revision);
    platform_lock();
    CHECK(!app_state.stale_handoffs[0].id[0]);
    platform_unlock();

    // Starting a playlist replaces the queue, and the saved queue survives a restart.
    CHECK(app_enqueue_item(&first) && app_enqueue_item(&third));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    platform_lock();
    app_up_next_save_locked(doc, root);
    platform_unlock();
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    CHECK(json != NULL);

    platform_lock();
    EngineState previous_engine = app_state.view.engine;
    app_state.view.engine = ENGINE_STARTING;
    platform_unlock();
    CatalogItem playlist = {.kind = ITEM_PLAYLIST, .id = "0000000000000000000070", .title = "Mix"};
    app_open_item(&playlist);
    CatalogItem other = queue_song(4);
    CHECK(app_play_search_history(&other));
    app_snapshot_into(view);
    CHECK(view->queue.queued == 0);

    yyjson_doc *saved = yyjson_read(json, strlen(json), 0);
    platform_lock();
    app_up_next_restore_locked(yyjson_doc_get_root(saved));
    platform_unlock();
    yyjson_doc_free(saved);
    free(json);
    app_snapshot_into(view);
    CHECK(view->queue.queued == 2 && !strcmp(view->queue.items[1].id, third.id));

    platform_lock();
    app_state.has_deferred = false;
    app_state.view.engine = previous_engine;
    platform_unlock();
    free(view);
    app_reset_session();
}

static void test_playlist_queue_and_cache(void) {
    BrowseRequest request = {.kind = BROWSE_PLAYLIST_TRACKS, .id = "0000000000000000000060", .title = "Long"};
    BrowsePage *page = calloc(1, sizeof(*page));
    CHECK(page != NULL);

    platform_lock();
    app_state.view.authenticated = true;
    app_state.view.list = (ListView){.request = request, .selected = -1};
    EngineState previous_engine = app_state.view.engine;
    app_state.view.engine = ENGINE_STARTING;

    // 120 songs arrive in pages, as a long playlist would.
    for (unsigned offset = 0; offset < 120; offset += BROWSE_PAGE_SIZE) {
        *page = (BrowsePage){.offset = offset, .limit = BROWSE_PAGE_SIZE, .total = 120, .has_next = offset < 110};

        for (unsigned i = 0; i < BROWSE_PAGE_SIZE; ++i) {
            page->items[page->count++] = queue_song(0);
            snprintf(page->items[i].id, sizeof(page->items[i].id), "0000000000000000001%03u", offset + i);
        }

        CHECK(app_collection_store(&request, page, offset > 0));
    }

    platform_unlock();

    // Playing the playlist shows every remaining song, not the ~20 Spotify reports.
    CHECK(app_play_collection());
    platform_lock();
    snprintf(app_state.view.playback.uri, sizeof(app_state.view.playback.uri), "spotify:track:%s",
             "0000000000000000001000");
    snprintf(app_state.view.playback.context_uri, sizeof(app_state.view.playback.context_uri),
             "spotify:playlist:%s", request.id);
    app_state.view.queue = (Queue){.count = 2};
    app_state.view.queue.items[0] = queue_song(98);
    app_state.view.queue.items[1] = queue_song(99);
    platform_unlock();

    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);
    app_snapshot_into(view);
    CHECK(view->queue.total == 119 && view->queue.first == 0 && view->queue.count == QUEUE_LIMIT);
    CHECK(!strcmp(queue_row(&view->queue, 0)->id, "0000000000000000001001"));

    // The panel scrolls through all of it; the window follows the scroll position.
    app_queue_viewport(100);
    app_snapshot_into(view);
    CHECK(view->queue.first == 92 && !strcmp(queue_row(&view->queue, 118)->id, "0000000000000000001119"));
    CHECK(!queue_row(&view->queue, 10));

    // Further into the playlist, the queue continues from the current song.
    platform_lock();
    snprintf(app_state.view.playback.uri, sizeof(app_state.view.playback.uri), "spotify:track:%s",
             "0000000000000000001050");
    platform_unlock();
    app_queue_viewport(0);
    app_snapshot_into(view);
    CHECK(view->queue.total == 69 && !strcmp(queue_row(&view->queue, 0)->id, "0000000000000000001051"));

    CatalogItem later = *queue_row(&view->queue, 20);
    CHECK(app_play_upcoming(&later));
    app_snapshot_into(view);
    CHECK(view->queue.total == 48 && !strcmp(queue_row(&view->queue, 0)->id, "0000000000000000001072"));

    // Playing something unrelated falls back to Spotify's own upcoming songs.
    platform_lock();
    app_state.view.playback.context_uri[0] = 0;
    platform_unlock();
    app_snapshot_into(view);
    CHECK(view->queue.total == 2 && !strcmp(queue_row(&view->queue, 0)->id, "0000000000000000000098"));

    // A fully loaded playlist is remembered and shown instantly the next time it opens.
    platform_lock();
    app_state.track_cache[0] = (TrackCacheEntry){.items = malloc(120 * sizeof(CatalogItem)), .count = 120,
                                                 .snapshot = "abc", .used = 1};
    CHECK(app_state.track_cache[0].items != NULL);
    memcpy(app_state.track_cache[0].items, app_state.collection, 120 * sizeof(CatalogItem));
    copy_utf8(app_state.track_cache[0].id, sizeof(app_state.track_cache[0].id), request.id);
    platform_unlock();

    CatalogItem other = {.kind = ITEM_PLAYLIST, .id = "0000000000000000000061", .title = "Other"};
    CHECK(app_open_item(&other));
    app_snapshot_into(view);
    CHECK(view->list.count == 0);

    CatalogItem cached = {.kind = ITEM_PLAYLIST, .id = "0000000000000000000060", .title = "Long"};
    CHECK(app_open_item(&cached));
    app_snapshot_into(view);
    CHECK(view->list.loaded && view->list.count == 120 && !view->list.more);
    wait_loaded();

    platform_lock();
    app_state.has_deferred = false;
    app_state.view.engine = previous_engine;
    platform_unlock();
    free(page);
    free(view);
    app_reset_session();
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
    CHECK(scene.hits[HIT_CARD_FIRST].rect.h > scene.hits[HIT_CARD_FIRST].rect.w);

    for (unsigned i = 0; i < 40; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    scene_layout(&scene, &view, 960, 640);
    HitId bottom_id = HIT_CARD_FIRST + 5 * HOME_SHELF_COLUMNS;
    Rect bottom = scene.hits[bottom_id].rect;
    CHECK(bottom.w > 100 && bottom.y + bottom.h <= scene.content.y + scene.content.h - 16);
    CHECK(scene_hit(&scene, bottom.x + 16, scene.player.y + 4) != bottom_id);
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
    scene_activate(&scene, &view, HIT_DEVICES);
    scene_layout(&scene, &view, 1600, 900);
    CHECK(scene.panel == PANEL_DEVICES);
    CHECK(scene.content.x + scene.content.w == scene.side_panel.x);
    scene_activate(&scene, &view, HIT_PANEL_CLOSE);
    scene_layout(&scene, &view, 1600, 900);
    CHECK(scene.panel == PANEL_NONE && scene.content.w > 1200);

    view.queue.count = 20;
    view.queue.total = 20;
    scene_activate(&scene, &view, HIT_QUEUE);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_QUEUE && scene.hits[HIT_PANEL_QUEUE].enabled);
    scene_activate(&scene, &view, HIT_PANEL_RECENT_TAB);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel_recent && scene.hits[HIT_PANEL_RECENT_FIRST].enabled);
    CHECK(!scene.hits[HIT_SCROLL_PANEL].enabled);
    scene_activate(&scene, &view, HIT_PANEL_QUEUE_TAB);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.panel_recent && scene.hits[HIT_SCROLL_PANEL].enabled);
    scene.over_panel = true;

    for (unsigned i = 0; i < 20; ++i) {
        scene_scroll(&scene, &view, 1);
    }

    CHECK(scene.panel_first > 0 && scene.home_scroll == 0);
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
    CHECK(scene.menu.open && scene.menu.count == 3 && !strcmp(scene.menu.labels[0], "Profile") &&
          !strcmp(scene.menu.labels[1], "Settings") && !strcmp(scene.menu.labels[2], "Log out"));
    scene_activate(&scene, &view, HIT_CTX_FIRST + 1);
    scene_layout(&scene, &view, 1280, 1100);
    CHECK(!scene.menu.open && !scene.home && scene.section == BROWSE_SETTINGS);
    CHECK(scene.hits[HIT_SETTING_SEARCH_RADIO].enabled);
    scene_activate(&scene, &view, HIT_BACK);
    CHECK(scene.section != BROWSE_SETTINGS);
    scene_activate(&scene, &view, HIT_FORWARD);
    CHECK(!scene.home && scene.section == BROWSE_SETTINGS);
    scene_activate(&scene, &view, HIT_PROFILE);
    scene_layout(&scene, &view, 1280, 1100);
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
        WindowsBuffer buffer = {.dc = paint, .pixels = pixels, .width = (int)width, .height = (int)height};
        windows_canvas_begin(&backend, &buffer, scales[scale], &canvas);
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
static void test_linux_rounded_artwork(void) {
    const float scales[] = {1, 1.25f, 1.5f, 2};
    uint32_t white[] = {0xffffff, 0xffffff, 0xffffff, 0xffffff};
    const Image source = {.pixels = white, .width = 2, .height = 2};
    Rect shape = {3.25f, 4.25f, 26, 18};
    LinuxCanvas backend = {0};

    for (unsigned i = 0; i < sizeof(scales) / sizeof(*scales); ++i) {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 128, 64);
        cairo_t *paint = cairo_create(surface);
        cairo_scale(paint, scales[i], scales[i]);
        Canvas canvas;
        linux_canvas_begin(&backend, paint, &canvas);
        canvas.box(canvas.context, (Rect){0, 0, 128, 64}, 0, 0);
        canvas.box(canvas.context, shape, 8, 0xffffff);
        shape.x += 32;
        linux_canvas_paint_image(&backend, &source, shape, 8);
        shape.x -= 32;
        cairo_surface_flush(surface);

        const uint32_t *pixels = (const uint32_t *)cairo_image_surface_get_data(surface);
        unsigned stride = (unsigned)cairo_image_surface_get_stride(surface) / 4;
        unsigned offset = (unsigned)(32 * scales[i]);
        unsigned partial = 0;

        for (unsigned y = 0; y < 64; ++y) {
            bool same = true;

            for (unsigned x = 0; x < offset; ++x) {
                uint32_t fill = pixels[y * stride + x] & 0xffffff;
                uint32_t art = pixels[y * stride + x + offset] & 0xffffff;
                same = same && fill == art;
                partial += fill > 0 && fill < 0xffffff;
            }

            CHECK(same);
        }

        CHECK(partial >= 20);
        linux_canvas_end(&backend);
        cairo_destroy(paint);
        cairo_surface_destroy(surface);
    }

    linux_canvas_destroy(&backend);
}

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

    for (unsigned state = 0; state < 25; ++state) {
        unsigned width = state == 9                                               ? 1600
                         : state == 2 || state == 5 || state == 10 || state == 11 ? 960
                                                                                  : 1280;
        unsigned height = state == 9                                               ? 900
                          : state == 2 || state == 5 || state == 10 || state == 11 ? 640
                                                                                   : 800;
        scene.menu = (ContextMenu){0};
        scene.home = state == 3 || (state >= 9 && state <= 15) || state == 19 || state >= 22;
        scene.library_collapsed = state == 23;
        scene.library_width = state == 24 ? 300 : 0;
        scene.panel_width = state == 24 ? 400 : 0;
        scene.hover[HIT_PANEL_RESIZE] = state == 24 ? 1 : 0;
        scene.home_scroll = state == 11 ? UINT32_MAX : 0;
        scene.main_scroll.offset = state == 11 ? 100000.0f : state == 8 ? 340.0f : 0;
        scene.main_scroll.target = scene.main_scroll.offset;
        scene.panel = state == 12 || state == 4    ? PANEL_DEVICES
                      : state == 13                ? PANEL_FRIENDS
                      : state == 14                ? PANEL_LYRICS
                      : state == 15 || state == 24 ? PANEL_QUEUE
                                                   : PANEL_NONE;
        scene.setup_popup = state == 6;
        scene.search_history_open = state == 19;
        view.engine = state == 6 ? ENGINE_NEEDS_KEY : ENGINE_OFF;
        scene.section = state == 7 || state == 8 ? BROWSE_SEARCH
                        : state == 16            ? BROWSE_ALBUM_TRACKS
                        : state == 17            ? BROWSE_ARTIST_ALBUMS
                        : state == 18            ? BROWSE_PROFILE_PLAYLISTS
                        : state == 20            ? BROWSE_SETTINGS
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
            view.search_playlist_count = 5;
            snprintf(scene.query, sizeof(scene.query), "label:stereofox");

            for (unsigned i = 0; i < 5; ++i) {
                view.search_artists[i] = (CatalogItem){.kind = ITEM_ARTIST, .id = "0123456789abcdefghijkl"};
                snprintf(view.search_artists[i].title, sizeof(view.search_artists[i].title),
                         "Fixture artist %u", i + 1);
                view.search_albums[i] = view.library[1].items[0];
                view.search_playlists[i] = view.library[2].items[0];
                snprintf(view.search_playlists[i].title, sizeof(view.search_playlists[i].title),
                         "Fixture playlist %u", i + 1);

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
            if (state == 19) {
                scene.query[0] = 0;
                snprintf(view.playback.uri, sizeof(view.playback.uri),
                         "spotify:track:0123456789abcdefghijkl");
                snprintf(view.playback.artist, sizeof(view.playback.artist), "Björk");
                view.playback_liked = true;
                view.search_history_count = 3;

                for (unsigned i = 0; i < 3; ++i) {
                    view.search_history[i] = page.items[0];
                    snprintf(view.search_history[i].title, sizeof(view.search_history[i].title),
                             "Recent song %u", i + 1);
                    snprintf(view.search_history[i].detail, sizeof(view.search_history[i].detail),
                             "Fixture artist");
                }
            }
            view.queue.count = 20;
            view.queue.total = 20;

            for (unsigned i = 0; i < view.queue.count; ++i) {
                view.queue.items[i] = page.items[0];
                snprintf(view.queue.items[i].title, sizeof(view.queue.items[i].title),
                         "Fixture queue song %u", i + 1);
            }
        }
        view.loading_intro = state == 21;
        view.authenticated = state != 1 && state != 2;
        scene_layout(&scene, &view, (float)width, (float)height);

        if (state == 22) {
            scene_activate(&scene, &view, HIT_PROFILE);
            scene_layout(&scene, &view, (float)width, (float)height);
        }

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
        if (scene.intro_screen) {
            CHECK((pixels[0] & 0xffffff) == DARK_BODY);
        } else if (scene.login_screen) {
            CHECK((pixels[(unsigned)panel.y * stride + (unsigned)panel.x] & 0xffffff) == DARK_BODY);
            check_text_center(pixels, stride, scene.hits[HIT_ACCOUNT].rect, true);
            check_text_center(pixels, stride, (Rect){panel.x + 32, panel.y + 108, panel.w - 64, 40}, false);
        } else {
            CHECK((pixels[0] & 0xffffff) == DARK_CHROME);
            CHECK((pixels[(unsigned)scene.player.y * stride] & 0xffffff) == DARK_BORDER);
            CHECK((pixels[((unsigned)scene.player.y + 1) * stride] & 0xffffff) == DARK_CHROME);

            unsigned edge =
                pixels[(unsigned)scene.side_panel.y * stride + (unsigned)scene.side_panel.x] & 0xffffff;

            // A hovered edge is drawn as a lighter line over the panel border.
            if (scene.panel != PANEL_NONE && scene.hover[HIT_PANEL_RESIZE] == 0) {
                CHECK(edge == DARK_BORDER);
            }

            if (scene.panel != PANEL_NONE && scene.hover[HIT_PANEL_RESIZE] == 1) {
                CHECK((edge & 0xff) > (DARK_BORDER & 0xff));
            }
        }

        if (scene.setup_popup) {
            check_text_center(pixels, stride, scene.hits[HIT_SETUP_PASTE].rect, true);
        }

        if (scene.menu.open && scene.menu.count >= 2 && scene.menu.actions[1] == CTX_SETTINGS) {
            Rect row = scene.hits[HIT_CTX_FIRST + 1].rect;
            unsigned icx = (unsigned)(row.x + 19.0f);
            unsigned icy = (unsigned)(row.y + row.h * 0.5f);
            CHECK((pixels[icy * stride + icx] & 0xffffff) == DARK_TILE);
            bool has_top_tooth = false;

            for (unsigned dy = 6; dy <= 8; ++dy) {
                if ((pixels[(icy - dy) * stride + icx] & 0xff) > 0x70) {
                    has_top_tooth = true;
                }
            }

            CHECK(has_top_tooth);
        }

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
                "native-profile-detail-fixture.png",  "native-player-like-recent-fixture.png",
                "native-settings-fixture.png",        "native-intro-fixture.png",
                "native-profile-menu-fixture.png",    "native-collapsed-library-fixture.png",
                "native-resized-panels-fixture.png"};
            snprintf(path, sizeof(path), "%s/%s", directory, names[state]);
            CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
        }

        cairo_surface_destroy(surface);
    }

    linux_canvas_destroy(&backend);
    test_linux_rounded_artwork();
    test_navigation_rendering();
    pango_cairo_font_map_set_default(NULL);
    cairo_debug_reset_static_data();
    FcConfigAppFontClear(FcConfigGetCurrent());
    FcFini();
}
#endif

#ifdef _WIN32
static void test_windows_frame_pixels(void) {
    WindowsBuffer frame = {0};
    WindowsCanvas backend = {0};
    Canvas canvas;
    const uint32_t background = 0x15181c;
    uint32_t pixels[] = {0xff0000, 0x00ff00, 0x0000ff, 0xffff00};
    const Image image = {.width = 2, .height = 2, .pixels = pixels};

    for (int width = 80; width <= 100; width += 20) {
        CHECK(windows_buffer_resize(&frame, NULL, width, 60));
        HBITMAP original = frame.bitmap;
        CHECK(windows_buffer_resize(&frame, NULL, width, 60) && frame.bitmap == original);
        windows_canvas_begin(&backend, &frame, 1.25f, &canvas);
        canvas.box(canvas.context, (Rect){0, 0, 80, 48}, 0, background);
        canvas.clip(canvas.context, (Rect){0, 0, 64, 48}, 0);
        canvas.gradient(canvas.context, (Rect){36, 4, 20, 32}, 5, 0xff0000, 0x0000ff);
        CHECK(windows_canvas_paint_image(&backend, &image, (Rect){4, 4, 32, 32}, 6));
        canvas.unclip(canvas.context);
        GdiFlush();

        CHECK((frame.pixels[5 * width + 5] & 0xffffff) == background);
        CHECK((frame.pixels[12 * width + 12] & 0xffffff) == 0xff0000);
        CHECK((frame.pixels[12 * width + 37] & 0xffffff) == 0x00ff00);
        CHECK((frame.pixels[37 * width + 12] & 0xffffff) == 0x0000ff);
        CHECK((frame.pixels[37 * width + 37] & 0xffffff) == 0xffff00);
        CHECK(GetPixel(frame.dc, 12, 37) == RGB(0, 0, 255));
        CHECK((frame.pixels[10 * width + 55] & 0xff0000) > 0xc00000);
        CHECK((frame.pixels[40 * width + 55] & 0xff) > 0xc0);
    }

    windows_canvas_destroy(&backend);
    windows_buffer_destroy(&frame);
}

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
        WindowsBuffer buffer = {
            .dc = paints[i], .pixels = pixels[i], .width = 256 * scale, .height = 128 * scale};
        windows_canvas_begin(&backends[i], &buffer, (float)scale, &canvases[i]);
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

static void test_controller_and_raster(void) {
    UiController controller;
    ui_controller_init(&controller, 1000);
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_ARROW);
    ui_controller_layout(&controller, 1280, 800);
    CHECK(controller.scene.login_screen);
    CHECK(controller.scene.focus == HIT_CLIENT_ID && ui_controller_is_editing(&controller));
    CHECK(ui_controller_shortcut(&controller, 'l', true) == UI_SHORTCUT_HANDLED);
    CHECK(controller.scene.focus == HIT_CLIENT_ID && ui_controller_is_editing(&controller));
    controller.scene.focus = HIT_ACCOUNT;
    CHECK(!ui_controller_is_editing(&controller));

    controller.scene.hot = HIT_SEARCH;
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_IBEAM);
    controller.scene.hot = HIT_HOME;
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_HAND);

    controller.scene.login_screen = false;
    controller.scene.focus = HIT_NONE;
    CHECK(ui_controller_shortcut(&controller, 'L', true) == UI_SHORTCUT_HANDLED);
    CHECK(controller.scene.focus == HIT_SEARCH && controller.scene.select_all);
    CHECK(ui_controller_is_editing(&controller));
    CHECK(ui_controller_text(&controller, "bach", 4));
    CHECK(!strcmp(controller.scene.query, "bach") && !controller.scene.select_all);
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    CHECK(controller.scene.select_all);
    CHECK(ui_controller_shortcut(&controller, 'v', true) == UI_SHORTCUT_PASTE_SEARCH);
    CHECK(ui_controller_shortcut(&controller, 'u', true) == UI_SHORTCUT_HANDLED);
    CHECK(!controller.scene.query[0]);

    bool cursor_changed = false;
    CHECK(ui_controller_motion(&controller, 0, 0, 1016, &cursor_changed));
    CHECK(cursor_changed && ui_controller_cursor(&controller) == UI_CURSOR_ARROW);
    ui_controller_leave(&controller, 1032);
    CHECK(controller.scene.hot == HIT_NONE);

    uint32_t pixels[64 * 64] = {0};
    RasterSurface surface = {
        .pixels = pixels,
        .width = 64,
        .height = 64,
        .clip = {0, 0, 64, 64},
        .scale = 1.0f,
    };
    canvas_raster_round_rect(&surface, (Rect){8, 8, 32, 32}, 8, 0xffffff, 0xffffff);
    CHECK(pixels[24 * 64 + 24] == 0xffffff);
    CHECK(pixels[8 * 64 + 8] == 0);

    canvas_raster_ellipse(&surface, (Rect){10, 10, 20, 20}, false, 2, 0x1ed760);
    CHECK(pixels[10 * 64 + 20] != 0);

    Point tri[3] = {{40, 40}, {56, 40}, {48, 56}};
    canvas_raster_path(&surface, tri, 3, true, true, 1, 0x336699);
    CHECK(pixels[44 * 64 + 48] == 0x336699);

    memset(pixels, 0, sizeof(pixels));
    canvas_raster_round_rect(&surface, (Rect){2.5f, 2.5f, 5, 5}, 0, 0xffffff, 0xffffff);
    CHECK(pixels[2 * 64 + 2] == 0x404040);

    const float scales[] = {1, 1.25f, 1.5f, 2};
    uint32_t clipped[64 * 64];
    uint32_t solid[] = {0xffffff, 0xffffff, 0xffffff, 0xffffff};
    const Image image = {.pixels = solid, .width = 2, .height = 2};
    const RasterRoundClip round = {.rect = {3.25f, 4.25f, 26, 18}, .radius = 8};

    for (unsigned i = 0; i < sizeof(scales) / sizeof(*scales); ++i) {
        memset(pixels, 0, sizeof(pixels));
        memset(clipped, 0, sizeof(clipped));
        surface.scale = scales[i];
        canvas_raster_round_rect(&surface, round.rect, round.radius, 0xffffff, 0xffffff);

        RasterSurface mask = surface;
        mask.pixels = clipped;
        mask.round_clips = &round;
        mask.round_clip_depth = 1;
        canvas_raster_image(&mask, &image, round.rect, round.radius);
        CHECK(!memcmp(pixels, clipped, sizeof(pixels)));

        unsigned partial = 0;

        for (unsigned p = 0; p < 64 * 64; ++p) {
            if (pixels[p] && pixels[p] != 0xffffff) {
                ++partial;
            }
        }

        CHECK(partial >= 20);
    }

    surface.scale = 1;
    memset(pixels, 0, sizeof(pixels));
    const RasterRoundClip left_strip = {.rect = {4, 4, 0.4f, 8}};
    surface.round_clips = &left_strip;
    surface.round_clip_depth = 1;
    canvas_raster_round_rect(&surface, (Rect){4.6f, 4, 8, 8}, 0, 0xffffff, 0xffffff);
    CHECK(pixels[5 * 64 + 4] == 0);
    surface.round_clip_depth = 0;

    uint32_t landscape[] = {0xff0000, 0x00ff00, 0x00ff00, 0x0000ff, 0xff0000, 0x00ff00, 0x00ff00, 0x0000ff};
    const Image wide = {.pixels = landscape, .width = 4, .height = 2};
    canvas_raster_image(&surface, &wide, (Rect){12, 12, 2, 2}, 0);
    CHECK(pixels[12 * 64 + 12] == 0x00ff00 && pixels[13 * 64 + 13] == 0x00ff00);

    canvas_raster_ellipse(&surface, (Rect){20, 20, 22, 12}, true, 0, 0xffffff);
    CHECK(pixels[26 * 64 + 31] == 0xffffff && pixels[20 * 64 + 20] == 0);
    uint32_t edge = pixels[20 * 64 + 25];
    CHECK(edge > 0 && edge < 0xffffff && edge == pixels[20 * 64 + 36]);
    ui_controller_destroy(&controller);
    app_reset_session();
}

static void test_loading_placeholders(void) {
    const BrowseKind kinds[] = {BROWSE_SAVED_TRACKS, BROWSE_SAVED_ALBUMS, BROWSE_ALBUM_TRACKS,
                                BROWSE_ARTIST_ALBUMS, BROWSE_PROFILE_PLAYLISTS, BROWSE_SEARCH};

    for (unsigned i = 0; i < sizeof(kinds) / sizeof(*kinds); ++i) {
        Scene scene;
        scene_init(&scene);
        scene.home = false;
        scene.section = kinds[i];
        View view = {.authenticated = true, .list = {.loading = true, .selected = -1}};
        view.list.request.kind = kinds[i];
        scene_layout(&scene, &view, 1280, 800);
        Hit row = scene.hits[HIT_ROW_FIRST];
        CHECK(row.rect.w > 0 && !row.enabled);
        CHECK(scene_hit(&scene, row.rect.x + 1, row.rect.y + 1) != HIT_ROW_FIRST);
        CHECK(!scene.animating);

        view.list.loading = false;
        view.list.loaded = true;
        scene_layout(&scene, &view, 1280, 800);
        CHECK(scene.hits[HIT_ROW_FIRST].rect.w == 0);

        if (kinds[i] == BROWSE_SAVED_ALBUMS) {
            view.list.count = 1;
            view.list.more = true;
            view.list.loading = true;
            scene_layout(&scene, &view, 1280, 800);
            CHECK(scene.hits[HIT_ROW_FIRST].enabled);
            CHECK(scene.hits[HIT_ROW_FIRST + 1].rect.w > 0 && !scene.hits[HIT_ROW_FIRST + 1].enabled);
        }
    }
}

static void test_play_pause_burst(void) {
    Worker *worker = &app_state.workers[LANE_PLAYER];
    platform_lock();
    worker->running = true; // Keep the real submission queue without starting network I/O.
    app_state.view.authenticated = true;
    app_state.view.playback = (Playback){.has_device = true, .device_id = "fixture"};
    platform_unlock();

    View *snapshot = malloc(sizeof(*snapshot));
    CHECK(snapshot != NULL);

    for (unsigned i = 0; i < 40; ++i) {
        app_action(ACTION_TOGGLE_PLAYBACK);
        app_snapshot_into(snapshot);
        CHECK(snapshot->playback.playing == (i % 2 == 0));
    }

    free(snapshot);

    CHECK(worker->count == 1 && worker->jobs[worker->head].action == ACTION_PAUSE);
    CHECK(app_submit((Job){.action = ACTION_NEXT}));
    app_action(ACTION_TOGGLE_PLAYBACK);
    CHECK(worker->count == 3);
    CHECK(worker->jobs[(worker->head + 1) % LANE_CAPACITY].action == ACTION_NEXT);
    CHECK(worker->jobs[(worker->head + 2) % LANE_CAPACITY].action == ACTION_PLAY);

    platform_lock();
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_space_toggles_playback(void) {
    Worker *worker = &app_state.workers[LANE_PLAYER];
    platform_lock();
    worker->running = true;
    app_state.view.authenticated = true;
    app_state.view.playback = (Playback){.has_device = true, .device_id = "fixture"};
    platform_unlock();

    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    scene_layout(&scene, &view, 1280, 800);

    // Space plays even when a control holds the focus, instead of pressing that control.
    scene.focus = HIT_HOME;
    CHECK(scene_key(&scene, &view, KEY_SPACE, false));
    CHECK(worker->count == 1 && worker->jobs[worker->head].action == ACTION_PLAY);
    CHECK(scene.home && scene.focus == HIT_HOME);

    // In a text field Space is typed, never a playback shortcut.
    scene.focus = HIT_LIST_FILTER;
    CHECK(scene_key(&scene, &view, KEY_SPACE, false) && worker->count == 1);
    scene.focus = HIT_SEARCH;
    CHECK(scene_key(&scene, &view, KEY_SPACE, false) && worker->count == 1);

    scene.focus = HIT_NONE;
    scene.menu.open = true;
    CHECK(!scene_key(&scene, &view, KEY_SPACE, false) && worker->count == 1);

    platform_lock();
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_sidebar_resize_and_collapse(void) {
    Scene scene;
    scene_init(&scene);
    View view = {.authenticated = true};
    view.playlists.count = 3;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.w == 223 && !scene.library_collapsed);

    // The library edge sits right of the sidebar, below the header, and wins over the scrollbar.
    Rect edge = scene.hits[HIT_LIBRARY_RESIZE].rect;
    CHECK(edge.x == scene.sidebar.w && edge.y == scene.header.h);
    CHECK(scene_hit(&scene, edge.x + 2, 400) == HIT_LIBRARY_RESIZE);
    CHECK(scene_hit(&scene, edge.x + 2, 20) != HIT_LIBRARY_RESIZE);

    UiController controller;
    ui_controller_init(&controller, 0);
    controller.scene.hot = HIT_LIBRARY_RESIZE;
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_RESIZE);
    ui_controller_destroy(&controller);

    // Dragging follows the pointer from where the edge was grabbed, within the allowed range.
    scene_press(&scene, &view, edge.x + 2, 400, 100);
    CHECK(scene.dragging == HIT_LIBRARY_RESIZE);
    scene_pointer_motion(&scene, 302, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.w == 300 && scene.content.x == 300);
    scene_pointer_motion(&scene, 1000, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.w == scene_library_max(1280));
    scene_pointer_motion(&scene, 302, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.sidebar.w == 300);
    scene_release(&scene, &view, 302, 400);
    CHECK(scene.dragging == HIT_NONE && app_snapshot().settings.library_width == 300);

    // Narrower than the midpoint snaps to the collapsed strip; artwork stays clickable.
    edge = scene.hits[HIT_LIBRARY_RESIZE].rect;
    scene_press(&scene, &view, edge.x + 2, 400, 200);
    scene_pointer_motion(&scene, 100, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.library_collapsed && scene.sidebar.w == LIBRARY_COLLAPSED_WIDTH);
    scene_release(&scene, &view, 100, 400);
    CHECK(app_snapshot().settings.library_collapsed);
    CHECK(!scene.hits[HIT_PLAYLISTS].enabled && !scene.hits[HIT_REFRESH].enabled);
    CHECK(scene.hits[HIT_TRACKS].enabled && scene.hits[HIT_LIBRARY_FIRST].enabled);
    CHECK(scene.hits[HIT_LIBRARY_FIRST].rect.x + scene.hits[HIT_LIBRARY_FIRST].rect.w <= scene.sidebar.w);

    // Clicking the library icon expands it again at the width it had.
    Rect toggle = scene.hits[HIT_LIBRARY_COLLAPSE].rect;
    scene_press(&scene, &view, toggle.x + 4, toggle.y + 4, 400);
    scene_release(&scene, &view, toggle.x + 4, toggle.y + 4);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(!scene.library_collapsed && scene.sidebar.w == 300);

    // The right panel keeps toggling and resizes from its left edge.
    scene_activate(&scene, &view, HIT_QUEUE);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_QUEUE && scene.side_panel.w == 320);
    Rect panel_edge = scene.hits[HIT_PANEL_RESIZE].rect;
    CHECK(scene_hit(&scene, scene.side_panel.x + 2, 400) == HIT_PANEL_RESIZE);
    scene_press(&scene, &view, scene.side_panel.x + 2, 400, 500);
    CHECK(scene.dragging == HIT_PANEL_RESIZE);
    scene_pointer_motion(&scene, scene.side_panel.x - 78, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.side_panel.w == 400 && scene.content.x + scene.content.w == scene.side_panel.x);
    scene_pointer_motion(&scene, 0, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.content.w >= CONTENT_MIN_WIDTH && scene.side_panel.w <= PANEL_MAX_WIDTH);
    scene_pointer_motion(&scene, 1270, 400);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.side_panel.w == PANEL_MIN_WIDTH);
    scene_release(&scene, &view, 1270, 400);
    CHECK(app_snapshot().settings.panel_width == PANEL_MIN_WIDTH);
    CHECK(panel_edge.h == scene.side_panel.h);

    scene_activate(&scene, &view, HIT_PANEL_CLOSE);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.panel == PANEL_NONE && !scene.hits[HIT_PANEL_RESIZE].enabled);

    // A new scene restores the saved layout.
    Scene restored;
    scene_init(&restored);
    View saved = app_snapshot();
    saved.authenticated = true;
    scene_layout(&restored, &saved, 1280, 800);
    CHECK(restored.sidebar.w == 300 && restored.panel_width == PANEL_MIN_WIDTH);
    app_set_layout(0, false, 0);
}

static void test_recent_search_popup(void) {
    Scene scene;
    scene_init(&scene);
    View view = {.authenticated = true, .search_history_count = SEARCH_HISTORY_LIMIT};
    scene_layout(&scene, &view, 1280, 800);
    float x = scene.search.x + 10;
    float y = scene.search.y + 8;
    scene_press(&scene, &view, x, y, 100);
    scene_release(&scene, &view, x, y);
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.search_history_open);
    CHECK(scene.search_history_panel.x == scene.search.x && scene.search_history_panel.w == scene.search.w);
    CHECK(scene.search_history_scroll.extent == 5 * HISTORY_ROW_HEIGHT);
    CHECK(scene.hits[HIT_HISTORY_FIRST + 4].enabled);
    CHECK(!scene.hits[HIT_HISTORY_FIRST + 5].enabled);

    Rect remove = scene.hits[HIT_HISTORY_REMOVE_FIRST].rect;
    CHECK(scene_hit(&scene, remove.x + 2, remove.y + 2) == HIT_HISTORY_REMOVE_FIRST);
    scene.over_search_history = true;
    scene_wheel(&scene, &view, .5f, 2, false);
    CHECK(scene.search_history_scroll.target == 0);
    CHECK(scene.main_scroll.target == 0);
    scene_press(&scene, &view, 10, 480, 120);
    CHECK(!scene.search_history_open);

    view.search_history_count = 0;
    scene.search_history_open = true;
    scene_layout(&scene, &view, 960, 640);
    CHECK(scene.search_history_panel.x == scene.search.x && scene.search_history_panel.w == scene.search.w);

    scene.home = false;
    Worker *worker = &app_state.workers[LANE_CONTENT];
    platform_lock();
    worker->running = true;
    platform_unlock();
    Rect top_home = scene.hits[HIT_TOP_HOME].rect;
    scene_press(&scene, &view, top_home.x + top_home.w * 0.5f, top_home.y + top_home.h * 0.5f, 140);
    scene_release(&scene, &view, top_home.x + top_home.w * 0.5f, top_home.y + top_home.h * 0.5f);
    CHECK(!scene.search_history_open && scene.home);
    CHECK(worker->count == 1 && worker->jobs[worker->head].action == ACTION_HOME);
    platform_lock();
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_wheel_settings(void) {
    Scene scene;
    scene_init(&scene);
    View view = {0};
    scene.home = true;
    scene.last_ms = 100;
    scroll_layout(&scene.main_scroll, 2400, 600);

    scene_wheel(&scene, &view, .25f, 2, false);
    CHECK(scene.main_scroll.target == 16);
    scene_wheel(&scene, &view, .75f, 2, false);
    CHECK(scene.main_scroll.target == 64);
    scene_wheel(&scene, &view, 1, 0, false);
    CHECK(scene.main_scroll.target == 64);
    scene_wheel(&scene, &view, 2, 0, true);
    CHECK(scene.main_scroll.target == 1264);
}

static void test_artist_likes(void) {
    CatalogItem artist = {.kind = ITEM_ARTIST, .id = "0000000000000000000001", .title = "Artist"};
    CatalogItem likes[] = {
        {.kind = ITEM_TRACK, .id = "0000000000000000000011", .artist_id = "0000000000000000000001"},
        {.kind = ITEM_TRACK, .id = "0000000000000000000012", .guest_artist_ids = {"0000000000000000000001"}},
        {.kind = ITEM_TRACK, .id = "0000000000000000000013", .artist_id = "0000000000000000000002"},
    };
    platform_lock();
    app_state.view.authenticated = true;
    app_state.likes = likes;
    app_state.likes_count = 3;
    Worker *worker = &app_state.workers[LANE_CONTENT];
    worker->running = true;
    app_state.view.artist_likes_failed = true;
    app_state.view.list.request = (BrowseRequest){.kind = BROWSE_ARTIST_ALBUMS};
    copy_utf8(app_state.view.list.request.id, sizeof(app_state.view.list.request.id), artist.id);
    copy_utf8(app_state.view.list.request.title, sizeof(app_state.view.list.request.title), artist.title);
    platform_unlock();

    Scene scene;
    scene_init(&scene);
    scene.session_revision = app_state.view.session_revision;
    scene.home = false;
    scene.section = BROWSE_ARTIST_ALBUMS;
    View view = app_snapshot();
    scene_layout(&scene, &view, 1280, 800);
    Rect hit = scene.hits[HIT_ARTIST_LIKES].rect;
    CHECK(scene_hit(&scene, hit.x + 2, hit.y + 2) == HIT_ARTIST_LIKES);
    scene_activate(&scene, &view, HIT_ARTIST_LIKES);
    view = app_snapshot();
    CHECK(view.list.request.kind == BROWSE_ARTIST_LIKES);
    CHECK(view.list.count == 2 && view.artist_liked_count == 2);
    CHECK(!view.artist_likes_ready && !view.artist_likes_failed);
    CHECK(list_row(&view.list, 0)->liked && list_row(&view.list, 1)->liked);
    CHECK(!strcmp(list_row(&view.list, 1)->id, likes[1].id));
    CHECK(worker->count == 1 && worker->jobs[worker->head].action == ACTION_LIKES);

    platform_lock();
    app_state.likes = NULL;
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_optimistic_likes(void) {
    const CatalogItem song = {.kind = ITEM_TRACK, .id = "0000000000000000000011"};
    Worker *worker = &app_state.workers[LANE_CONTENT];
    platform_lock();
    worker->running = true;
    app_state.view.authenticated = true;
    app_state.view.queue.count = 1;
    app_state.view.queue.items[0] = song;
    snprintf(app_state.view.playback.uri, sizeof(app_state.view.playback.uri), "spotify:track:%s", song.id);
    platform_unlock();

    CHECK(app_toggle_song_like(&song));
    View *view = malloc(sizeof(*view));
    CHECK(view != NULL);
    app_snapshot_into(view);
    CHECK(view->playback_liked && view->queue.items[0].liked);
    Job first = worker->jobs[worker->head];
    CHECK(first.value == 1);
    CHECK(app_toggle_song_like(&song));
    app_snapshot_into(view);
    CHECK(!view->playback_liked);
    Job second = worker->jobs[(worker->head + 1) % LANE_CAPACITY];
    CHECK(second.value == 0);

    app_finish_song_like(&first, "Write failed");
    app_snapshot_into(view);
    CHECK(!view->playback_liked);
    app_finish_song_like(&second, "Write failed");
    app_snapshot_into(view);
    CHECK(!view->queue.items[0].liked);
    CHECK(app_toggle_song_like(&song));
    app_snapshot_into(view);
    CHECK(view->playback_liked);
    app_finish_song_like(&worker->jobs[(worker->head + 2) % LANE_CAPACITY], "Write failed");
    app_snapshot_into(view);
    CHECK(!view->playback_liked);
    CHECK(!strcmp(view->status, "Write failed"));
    free(view);

    platform_lock();
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_lyrics_parser(void) {
    Playback track = {.uri = "spotify:track:fixture", .title = "Song", .artist = "Artist", .duration_ms = 30000};
    Lyrics lyrics = {0};
    const char *exact = "{\"syncedLyrics\":\"[ar:Artist]\\n[00:08.123][00:02.5]  Héllo\\r\\n"
                        "[00:04.00] \\n[00:42.00] Too late\\n[offset:100]\",\"plainLyrics\":\"Fallback\"}";
    CHECK(lyrics_parse(exact, strlen(exact), &track, false, &lyrics));
    CHECK(lyrics_match(&lyrics, &track) && lyrics.state == LYRICS_SYNCED && lyrics.count == 3);
    CHECK(lyrics.lines[0].time_ms == 2400 && lyrics.lines[2].time_ms == 8023);
    CHECK(!strcmp(lyrics.text + lyrics.lines[0].text, "Héllo"));
    CHECK(lyrics_active_line(&lyrics, 2399) == -1 && lyrics_active_line(&lyrics, 2400) == 0);
    CHECK(lyrics_active_line(&lyrics, 4000) == 1 && lyrics.text[lyrics.lines[1].text] == 0);
    CHECK(lyrics_active_line(&lyrics, 8023) == 2);

    const char *plain = "{\"syncedLyrics\":\"[bad] unparseable\",\"plainLyrics\":\"Line one\\n\\nLine two\"}";
    CHECK(lyrics_parse(plain, strlen(plain), &track, false, &lyrics));
    CHECK(lyrics.state == LYRICS_PLAIN && lyrics.count == 3 && lyrics_active_line(&lyrics, 2000) == -1);

    const char *search = "[{\"trackName\":\"Song\",\"artistName\":\"Artist\",\"duration\":38,"
                         "\"syncedLyrics\":\"[00:02.00] Wrong version\"},"
                         "{\"trackName\":\"song\",\"artistName\":\"ARTIST\",\"duration\":30.5,"
                         "\"syncedLyrics\":\"[00:03.25] Matching version\"}]";
    CHECK(lyrics_parse(search, strlen(search), &track, true, &lyrics));
    CHECK(lyrics.state == LYRICS_SYNCED && lyrics.count == 1 && lyrics.lines[0].time_ms == 3250);
}

static void test_lyrics_scene(Canvas *canvas) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
    copy_utf8(view.playback.uri, sizeof(view.playback.uri), "spotify:track:0123456789abcdefghijkl");
    copy_utf8(view.playback.artist, sizeof(view.playback.artist), "Björk");
    view.playback.playing = false;
    view.playback.position_ms = 4000;
    const char *json = "{\"syncedLyrics\":\"[00:01.00] A lyric line long enough to wrap in the sidebar"
                       "\\n[00:04.00] Second line\\n[00:06.47] Third line\"}";
    CHECK(lyrics_parse(json, strlen(json), &view.playback, false, &view.lyrics));
    scene_layout(&scene, &view, 1280, 800);
    scene_activate(&scene, &view, HIT_LYRICS);
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, canvas);
    CHECK(scene.panel == PANEL_LYRICS && scene.lyric_active == 1);
    CHECK(scene.lyric_y[1] - scene.lyric_y[0] > scene.lyric_y[2] - scene.lyric_y[1]);

    Worker *worker = &app_state.workers[LANE_PLAYER];
    platform_lock();
    worker->running = true;
    app_state.view.authenticated = true;
    app_state.view.playback = view.playback;
    platform_unlock();
    Rect line = scene.hits[HIT_LYRIC_FIRST + 2].rect;
    CHECK(scene_hit(&scene, line.x + 1, line.y + 1) == HIT_LYRIC_FIRST + 2);
    scene_press(&scene, &view, line.x + 1, line.y + 1, platform_milliseconds());
    scene_release(&scene, &view, line.x + 1, line.y + 1);
    CHECK(worker->count == 1 && worker->jobs[worker->head].action == ACTION_SEEK);
    {
        View *snapshot = malloc(sizeof(*snapshot));
        CHECK(snapshot != NULL);
        app_snapshot_into(snapshot);
        CHECK(worker->jobs[worker->head].value == 6470 && snapshot->playback.position_ms == 6470);
        view.playback.position_ms = snapshot->playback.position_ms;
        free(snapshot);
    }
    scene_activate(&scene, &view, HIT_LYRICS_FULLSCREEN);
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, canvas);
    CHECK(scene.lyrics_fullscreen && scene.side_panel.w == 1280 && scene.lyric_active == 2);
    CHECK(!scene.hits[HIT_SEARCH].enabled && scene.hits[HIT_PLAY].enabled);
    CHECK(scene_key(&scene, &view, KEY_ESCAPE, false));
    CHECK(!scene.lyrics_fullscreen && scene.panel == PANEL_LYRICS);

    json = "{\"plainLyrics\":\"Text only\"}";
    CHECK(lyrics_parse(json, strlen(json), &view.playback, false, &view.lyrics));
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, canvas);
    CHECK(scene.lyric_active == -1 && !scene.hits[HIT_LYRIC_FIRST].enabled);
    copy_utf8(view.playback.uri, sizeof(view.playback.uri), "spotify:track:another");
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.lyric_state == LYRICS_IDLE);

    platform_lock();
    worker->running = false;
    worker->count = 0;
    platform_unlock();
    app_reset_session();
}

static void test_player_like_and_header_layout(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    CHECK(playback_parse(PLAYER_FIXTURE, &view.playback));
    snprintf(view.playback.uri, sizeof(view.playback.uri), "spotify:track:0123456789abcdefghijkl");
    snprintf(view.playback.title, sizeof(view.playback.title), "Jóga");
    snprintf(view.playback.artist, sizeof(view.playback.artist), "Björk");
    ++view.playback_revision;

#ifdef _WIN32
    windows_load_font();
    HDC paint = CreateCompatibleDC(NULL);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 1280;
    info.bmiHeader.biHeight = -800;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *data = NULL;
    HBITMAP image = CreateDIBSection(paint, &info, DIB_RGB_COLORS, &data, NULL, 0);
    CHECK(paint && image && data);
    HGDIOBJ previous = SelectObject(paint, image);
    WindowsBuffer buffer = {.dc = paint, .pixels = data, .width = 1280, .height = 800};
    WindowsCanvas backend = {0};
    Canvas canvas;
    windows_canvas_begin(&backend, &buffer, 1.0f, &canvas);
#else
    linux_load_font();
    LinuxCanvas backend = {0};
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1280, 800);
    cairo_t *paint = cairo_create(surface);
    Canvas canvas;
    linux_canvas_begin(&backend, paint, &canvas);
#endif

    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, &canvas);
    Rect short_like = scene.hits[HIT_PLAYER_LIKE].rect;
    CHECK(scene.hits[HIT_PLAYER_LIKE].enabled);
    CHECK(short_like.x > 77.0f && short_like.x < 130.0f);
    CHECK(scene_hit(&scene, short_like.x + 14, short_like.y + 14) == HIT_PLAYER_LIKE);

    snprintf(view.playback.title, sizeof(view.playback.title), "Hi");
    snprintf(view.playback.artist, sizeof(view.playback.artist), "A Much Longer Artist Name That Extends");
    scene_layout(&scene, &view, 1280, 800);
    scene_draw(&scene, &view, &canvas);
    Rect artist_like = scene.hits[HIT_PLAYER_LIKE].rect;
    CHECK(artist_like.x > short_like.x && artist_like.x > 160.0f);

    snprintf(view.playback.title, sizeof(view.playback.title),
             "An Extremely Long Song Title That Truncates Cleanly At The Max Width");
    snprintf(view.playback.artist, sizeof(view.playback.artist), "Short");
    scene_layout(&scene, &view, 1000, 700);
    scene_draw(&scene, &view, &canvas);
    Rect clamped_like = scene.hits[HIT_PLAYER_LIKE].rect;
    CHECK(clamped_like.x + clamped_like.w <= scene.hits[HIT_SEEK].rect.x - 36.0f);

    /* Other like buttons stay in their original positions. */
    scene.panel = PANEL_DEVICES;
    view.list.loaded = true;
    view.list.request.kind = BROWSE_SAVED_TRACKS;
    view.list.count = 1;
    view.list.rows_count = 1;
    view.list.rows[0] =
        (CatalogItem){.kind = ITEM_TRACK, .id = "0123456789abcdefghijkl", .title = "Song", .playable = true};
    view.queue.count = 1;
    view.queue.total = 1;
    view.queue.items[0] = view.list.rows[0];
    view.search_history_count = 1;
    view.search_history[0] = view.list.rows[0];
    scene.search_history_open = true;
    scene.home = false;
    scene.section = BROWSE_SAVED_TRACKS;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_ROW_LIKE_FIRST].rect.x ==
          scene.hits[HIT_ROW_FIRST].rect.x + scene.hits[HIT_ROW_FIRST].rect.w - 96.0f);
    CHECK(scene.hits[HIT_HISTORY_LIKE_FIRST].rect.x ==
          scene.hits[HIT_HISTORY_FIRST].rect.x + scene.hits[HIT_HISTORY_FIRST].rect.w - 60.0f);

    scene.panel = PANEL_QUEUE;
    scene_layout(&scene, &view, 1280, 800);
    CHECK(scene.hits[HIT_QUEUE_LIKE_FIRST].rect.x == scene.side_panel.x + scene.side_panel.w - 76.0f);

    /* Search input and home button fit cleanly without colliding with Back/Forward/Profile. */
    const float widths[] = {960, 1080, 1280, 1600};

    for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        scene.panel = i % 2 ? PANEL_DEVICES : PANEL_NONE;
        scene_layout(&scene, &view, widths[i], 800);
        Rect fwd = scene.hits[HIT_FORWARD].rect;
        Rect th = scene.hits[HIT_TOP_HOME].rect;
        Rect prof = scene.hits[HIT_PROFILE].rect;
        CHECK(th.x >= fwd.x + fwd.w + 8.0f);
        CHECK(th.x + th.w < scene.search.x);
        CHECK(scene.search.x + scene.search.w <= prof.x - 8.0f);
    }

    test_lyrics_scene(&canvas);

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

static void test_playlist_sorting_and_settings(void) {
    Scene scene;
    scene_init(&scene);
    View *view = calloc(1, sizeof(*view));
    CHECK(view != NULL);

    app_snapshot_into(view);
    CHECK(view->settings.search_radio);
    view->authenticated = true;
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.hits[HIT_TRACKS].rect.y == 192.0f && scene.library_rows.y == 246.0f);

    /* Open Settings through Profile context menu and toggle search radio setting persistently. */
    scene_activate(&scene, view, HIT_PROFILE);
    scene_layout(&scene, view, 1280, 800);
    CHECK(scene.menu.open && scene.menu.count == 3 && !strcmp(scene.menu.labels[1], "Settings"));
    scene_activate(&scene, view, HIT_CTX_FIRST + 1);
    app_snapshot_into(view);
    view->authenticated = true;
    scene_layout(&scene, view, 1280, 800);
    CHECK(!scene.home && scene.section == BROWSE_SETTINGS);
    Rect toggle = scene.hits[HIT_SETTING_SEARCH_RADIO].rect;
    CHECK(scene_hit(&scene, toggle.x + 10, toggle.y + 10) == HIT_SETTING_SEARCH_RADIO);
    scene_activate(&scene, view, HIT_SETTING_SEARCH_RADIO);
    app_snapshot_into(view);
    CHECK(!view->settings.search_radio);
    app_initialize();
    app_snapshot_into(view);
    CHECK(!view->settings.search_radio);
    CHECK(app_set_search_radio(true));
    app_snapshot_into(view);
    CHECK(view->settings.search_radio);

    /* Playlist sorting in descending order of last played time. */
    CatalogItem p1 = {.kind = ITEM_PLAYLIST, .id = "0123456789abcdefghij01", .title = "Playlist One"};
    CatalogItem p2 = {.kind = ITEM_PLAYLIST, .id = "0123456789abcdefghij02", .title = "Playlist Two"};
    CatalogItem p3 = {.kind = ITEM_PLAYLIST, .id = "0123456789abcdefghij03", .title = "Playlist Three"};
    Worker *player = &app_state.workers[LANE_PLAYER];
    platform_lock();
    player->running = true;
    app_state.view.authenticated = true;
    app_state.view.engine = ENGINE_READY;
    snprintf(app_state.view.engine_device_id, sizeof(app_state.view.engine_device_id), "fixture");
    app_state.playlists = calloc(8, sizeof(*app_state.playlists));
    CHECK(app_state.playlists != NULL);
    app_state.playlist_capacity = 8;
    app_state.playlists[0] = p1;
    app_state.playlists[1] = p2;
    app_state.playlists[2] = p3;
    app_state.view.playlists.count = 3;
    app_state.view.playlists.total = 3;
    app_state.view.playlists.loaded = true;
    app_sort_playlists_locked(app_state.playlists, 3);
    platform_unlock();

    app_snapshot_into(view);
    CHECK(!strcmp(list_row(&view->playlists, 0)->id, p1.id));
    CHECK(!strcmp(list_row(&view->playlists, 1)->id, p2.id));
    CHECK(!strcmp(list_row(&view->playlists, 2)->id, p3.id));

    /* Browsing a playlist without playing keeps existing order; playing moves it to top. */
    platform_lock();
    app_state.view.list.loaded = true;
    app_state.view.list.request = (BrowseRequest){.kind = BROWSE_PLAYLIST_TRACKS};
    snprintf(app_state.view.list.request.id, sizeof(app_state.view.list.request.id), "%s", p3.id);
    app_sort_playlists_locked(app_state.playlists, 3);
    platform_unlock();
    app_snapshot_into(view);
    CHECK(!strcmp(list_row(&view->playlists, 0)->id, p1.id));

    CHECK(app_play_collection());
    app_snapshot_into(view);
    CHECK(!strcmp(list_row(&view->playlists, 0)->id, p3.id));
    CHECK(!strcmp(list_row(&view->playlists, 1)->id, p1.id));
    CHECK(!strcmp(list_row(&view->playlists, 2)->id, p2.id));

    platform_lock();
    snprintf(app_state.view.list.request.id, sizeof(app_state.view.list.request.id), "%s", p2.id);
    platform_unlock();
    CHECK(app_play_collection());
    app_snapshot_into(view);
    CHECK(!strcmp(list_row(&view->playlists, 0)->id, p2.id));
    CHECK(!strcmp(list_row(&view->playlists, 1)->id, p3.id));
    CHECK(!strcmp(list_row(&view->playlists, 2)->id, p1.id));

    /* Newly fetched library pages re-sort into persisted MRU order cleanly. */
    platform_lock();
    app_state.playlists[0] = p1;
    app_state.playlists[1] = p2;
    app_state.playlists[2] = p3;
    app_sort_playlists_locked(app_state.playlists, 3);
    platform_unlock();
    app_snapshot_into(view);
    CHECK(!strcmp(list_row(&view->playlists, 0)->id, p2.id));
    CHECK(!strcmp(list_row(&view->playlists, 1)->id, p3.id));
    CHECK(!strcmp(list_row(&view->playlists, 2)->id, p1.id));

    /* Playing a search song with repeat on records the seed so turning repeat off queues radio. */
    CatalogItem song = {
        .kind = ITEM_TRACK, .id = "0123456789abcdefghij99", .title = "Search Song", .playable = true};
    platform_lock();
    app_state.view.playback.repeat = REPEAT_TRACK;
    app_state.view.list.request = (BrowseRequest){.kind = BROWSE_SEARCH};
    player->head = 0;
    player->count = 0;
    platform_unlock();

    CHECK(app_open_item(&song));
    platform_lock();
    CHECK(player->count == 1 && player->jobs[player->head].action == ACTION_PLAY_ITEM);
    CHECK(player->jobs[player->head].browse.kind == BROWSE_SEARCH);
    CHECK(!strcmp(app_state.search_radio_seed.id, song.id));
    CHECK(!strcmp(app_state.view.playback.uri, "spotify:track:0123456789abcdefghij99"));
    platform_unlock();

    app_action(ACTION_REPEAT); /* track -> off */
    platform_lock();
    CHECK(player->count == 2 && player->jobs[(player->head + 1) % LANE_CAPACITY].action == ACTION_REPEAT);
    CHECK(player->jobs[(player->head + 1) % LANE_CAPACITY].value == REPEAT_OFF);
    CHECK(!strcmp(app_state.search_radio_seed.id, song.id));

    /* Playing an album track clears the search radio seed. */
    app_state.view.list.request = (BrowseRequest){.kind = BROWSE_ALBUM_TRACKS};
    snprintf(app_state.view.list.request.id, sizeof(app_state.view.list.request.id),
             "0123456789abcdefghij88");
    platform_unlock();
    CHECK(app_open_item(&song));
    platform_lock();
    CHECK(!app_state.search_radio_seed.id[0]);
    player->running = false;
    player->count = 0;
    free(app_state.playlists);
    app_state.playlists = NULL;
    app_state.playlist_capacity = 0;
    free(app_state.playlist_usage);
    app_state.playlist_usage = NULL;
    app_state.playlist_usage_count = 0;
    app_state.playlist_usage_capacity = 0;
    app_state.playlist_usage_clock = 0;
    platform_unlock();
    app_reset_session();
    free(view);
}

static void test_intro_session_restore_and_player_links(void) {
    /* Purple Spotify logo bitmap generator produces transparent corners, purple disc, and white waves. */
    uint32_t icon_pixels[64 * 64];
    canvas_brand_icon_pixels(64, icon_pixels);
    CHECK((icon_pixels[0] >> 24) == 0);
    bool has_purple = false;
    bool has_white = false;

    for (unsigned i = 0; i < 64 * 64; ++i) {
        uint32_t rgb = icon_pixels[i] & 0xffffffu;

        if (rgb == 0x7c3aedu && (icon_pixels[i] >> 24) == 255) {
            has_purple = true;
        }

        if (rgb == 0xffffffu && (icon_pixels[i] >> 24) == 255) {
            has_white = true;
        }
    }

    CHECK(has_purple && has_white);

    /* Intro screen disables interactive background hits until home + library readiness completes. */
    UiController controller;
    ui_controller_init(&controller, 1000);
    View *view = calloc(1, sizeof(*view));
    CHECK(view != NULL);
    view->loading_intro = true;
    view->authenticated = true;
    scene_layout(&controller.scene, view, 1280, 800);
    CHECK(controller.scene.intro_screen && !controller.scene.login_screen && controller.scene.animating);
    CHECK(!controller.scene.hits[HIT_HOME].enabled && !controller.scene.hits[HIT_PLAY].enabled &&
          !controller.scene.hits[HIT_ACCOUNT].enabled);
    CHECK(!ui_controller_is_editing(&controller) && ui_controller_cursor(&controller) == UI_CURSOR_ARROW);

    platform_lock();
    app_state.view.loading_intro = true;
    app_state.startup_home_done = true;
    app_state.startup_library_done = false;
    app_sync_startup_intro_locked();
    CHECK(app_state.view.loading_intro);
    app_state.startup_library_done = true;
    app_sync_startup_intro_locked();
    CHECK(!app_state.view.loading_intro);
    platform_unlock();

    /* Persist song, seek, queue, shuffle, repeat, and volume across relaunch cleanly. */
    CatalogItem q1 = {.kind = ITEM_TRACK,
                      .id = "0123456789abcdefghij11",
                      .title = "Queued One",
                      .detail = "Artist Q1",
                      .duration_ms = 180000,
                      .duration_known = true,
                      .playable = true};
    CatalogItem q2 = {.kind = ITEM_TRACK,
                      .id = "0123456789abcdefghij12",
                      .title = "Queued Two",
                      .detail = "Artist Q2",
                      .duration_ms = 210000,
                      .duration_known = true,
                      .playable = true};

    platform_lock();
    app_state.view.authenticated = true;
    playback_reset(&app_state.view.playback);
    copy_utf8(app_state.view.playback.uri, sizeof(app_state.view.playback.uri),
              "spotify:track:0123456789abcdefghij00");
    copy_utf8(app_state.view.playback.context_uri, sizeof(app_state.view.playback.context_uri),
              "spotify:playlist:0123456789abcdefghij55");
    copy_utf8(app_state.view.playback.title, sizeof(app_state.view.playback.title), "Remembered Song");
    copy_utf8(app_state.view.playback.artist, sizeof(app_state.view.playback.artist), "Remembered Artist");
    copy_utf8(app_state.view.playback.artist_id, sizeof(app_state.view.playback.artist_id),
              "0123456789abcdefghij77");
    copy_utf8(app_state.view.playback.album, sizeof(app_state.view.playback.album), "Remembered Album");
    copy_utf8(app_state.view.playback.album_id, sizeof(app_state.view.playback.album_id),
              "0123456789abcdefghij88");
    copy_utf8(app_state.view.playback.artwork, sizeof(app_state.view.playback.artwork),
              "https://i.scdn.co/image/ab0123");
    app_state.view.playback.position_ms = 91000;
    app_state.view.playback.duration_ms = 240000;
    app_state.view.playback.position_known = true;
    app_state.view.playback.playing = true;
    app_state.view.playback.shuffle = true;
    app_state.view.playback.repeat = REPEAT_CONTEXT;
    app_state.view.playback.volume_percent = 42;
    app_state.view.playback_received_ms = platform_milliseconds();
    app_state.view.queue = (Queue){.count = 2, .received = 2};
    app_state.view.queue.items[0] = q1;
    app_state.view.queue.items[1] = q2;
    app_save_playback_locked(true);
    platform_unlock();

    app_reset_session();
    app_initialize();
    app_snapshot_into(view);
    CHECK(!strcmp(view->playback.uri, "spotify:track:0123456789abcdefghij00"));
    CHECK(!strcmp(view->playback.context_uri, "spotify:playlist:0123456789abcdefghij55"));
    CHECK(!strcmp(view->playback.title, "Remembered Song"));
    CHECK(!strcmp(view->playback.artist, "Remembered Artist"));
    CHECK(!strcmp(view->playback.artist_id, "0123456789abcdefghij77"));
    CHECK(!strcmp(view->playback.album, "Remembered Album"));
    CHECK(!strcmp(view->playback.album_id, "0123456789abcdefghij88"));
    CHECK(view->playback.position_ms >= 91000 && view->playback.position_ms <= 93000);
    CHECK(view->playback.duration_ms == 240000 && view->playback.position_known);
    CHECK(!view->playback.playing && view->playback.shuffle && view->playback.repeat == REPEAT_CONTEXT &&
          view->playback.volume_percent == 42);
    CHECK(view->queue.count == 2 && !strcmp(view->queue.items[0].id, q1.id) &&
          !strcmp(view->queue.items[1].id, q2.id));
    CHECK(app_state.restored_pending);

    /* Clicking currently playing song opens album and clicking artist opens artist page, with pointer cursor.
     */
    controller.scene = (Scene){0};
    scene_init(&controller.scene);
    view->authenticated = true;
    scene_layout(&controller.scene, view, 1280, 800);
#ifndef _WIN32
    linux_load_font();
    LinuxCanvas backend = {0};
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1280, 800);
    cairo_t *paint = cairo_create(surface);
    Canvas canvas;
    linux_canvas_begin(&backend, paint, &canvas);
    scene_draw(&controller.scene, view, &canvas);
    linux_canvas_end(&backend);
    cairo_destroy(paint);
    cairo_surface_destroy(surface);
    linux_canvas_destroy(&backend);
#endif
    Rect song_hit = controller.scene.hits[HIT_PLAYER_SONG].rect;
    Rect artist_hit = controller.scene.hits[HIT_PLAYER_ARTIST].rect;
    Rect like_hit = controller.scene.hits[HIT_PLAYER_LIKE].rect;
    CHECK(controller.scene.hits[HIT_PLAYER_SONG].enabled && controller.scene.hits[HIT_PLAYER_ARTIST].enabled);
    CHECK(song_hit.y + song_hit.h <= artist_hit.y);
    CHECK(song_hit.x + song_hit.w <= like_hit.x && artist_hit.x + artist_hit.w <= like_hit.x);

    CHECK(scene_hit(&controller.scene, song_hit.x + 6, song_hit.y + 6) == HIT_PLAYER_SONG);
    scene_hover(&controller.scene, HIT_PLAYER_SONG, 2000);
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_HAND);

    CHECK(scene_hit(&controller.scene, artist_hit.x + 6, artist_hit.y + 6) == HIT_PLAYER_ARTIST);
    scene_hover(&controller.scene, HIT_PLAYER_ARTIST, 2000);
    CHECK(ui_controller_cursor(&controller) == UI_CURSOR_HAND);

    scene_activate(&controller.scene, view, HIT_PLAYER_ARTIST);
    wait_loaded();
    CHECK(!controller.scene.home && controller.scene.section == BROWSE_ARTIST_ALBUMS &&
          !strcmp(controller.scene.collection, "Remembered Artist"));

    scene_activate(&controller.scene, view, HIT_PLAYER_SONG);
    wait_loaded();
    CHECK(!controller.scene.home && controller.scene.section == BROWSE_ALBUM_TRACKS &&
          !strcmp(controller.scene.collection, "Remembered Album"));

    app_action(ACTION_DISCONNECT);
    wait_loaded();
    app_reset_session();
    ui_controller_destroy(&controller);
    free(view);
}

static void test_memory_bounds(void) {
    CHECK(LIST_LIMIT <= 1500);
    CHECK(LIKES_LIMIT <= 500);
    CHECK(PLAYLISTS_LIMIT <= 250);
    CHECK(PLAY_ORDER_LIMIT <= 128);
    CHECK(UP_NEXT_LIMIT <= 50);
    CHECK(TRACK_CACHE_SLOTS <= 1);
    CHECK(TRACK_CACHE_ITEM_LIMIT <= 150);
    CHECK(DETAIL_CACHE_LIMIT <= 2);
    CHECK(sizeof(CatalogItem) <= 1600);
    CHECK(sizeof(AppState) <= 1550000);

    app_reset_session();

    BrowsePage *page = calloc(1, sizeof(*page));
    CHECK(page != NULL);

    BrowseRequest first = {.kind = BROWSE_ALBUM_TRACKS, .id = "memtestalbum0000000001"};
    page->count = BROWSE_PAGE_SIZE;
    page->limit = BROWSE_PAGE_SIZE;
    page->total = BROWSE_PAGE_SIZE * 30;
    page->has_next = true;

    platform_lock();

    for (unsigned i = 0; i < 30; ++i) {
        page->offset = i * BROWSE_PAGE_SIZE;
        CHECK(app_collection_store(&first, page, i > 0));
    }

    CHECK(app_state.collection_count == BROWSE_PAGE_SIZE * 30);
    CHECK(app_state.collection_capacity > 256);

    BrowseRequest second = {.kind = BROWSE_ALBUM_TRACKS, .id = "memtestalbum0000000002"};
    *page = (BrowsePage){.count = 5, .limit = 5, .total = 5};
    CHECK(app_collection_store(&second, page, false));
    CHECK(app_state.collection_count == 5);
    CHECK(app_state.collection_capacity == 64);

    *page = (BrowsePage){0};
    CHECK(app_collection_store(&second, page, false));
    CHECK(app_state.collection == NULL && app_state.collection_capacity == 0 &&
          app_state.collection_count == 0);

    app_state.likes = calloc(4, sizeof(*app_state.likes));
    CHECK(app_state.likes != NULL);
    app_state.likes_capacity = 4;
    app_state.likes_count = 4;

    Worker *content = &app_state.workers[LANE_CONTENT];
    content->running = true;
    app_state.view.authenticated = true;
    platform_unlock();

    CHECK(app_browse(BROWSE_PLAYLISTS, NULL));

    platform_lock();
    content->running = false;
    content->count = 0;
    CHECK(app_state.likes == NULL && app_state.likes_capacity == 0 && app_state.likes_count == 0);

    app_state.play_order = calloc(4, sizeof(*app_state.play_order));
    CHECK(app_state.play_order != NULL);
    app_state.play_order_capacity = 4;
    app_state.play_order_count = 4;

    app_state.order = calloc(4, sizeof(*app_state.order));
    CHECK(app_state.order != NULL);
    app_state.order_capacity = 4;

    app_state.playlists = calloc(4, sizeof(*app_state.playlists));
    CHECK(app_state.playlists != NULL);
    app_state.playlist_capacity = 4;
    app_state.view.playlists.count = 4;
    platform_unlock();

    free(page);
    app_reset_session();

    platform_lock();
    CHECK(app_state.collection == NULL && app_state.collection_capacity == 0 &&
          app_state.collection_count == 0);
    CHECK(app_state.order == NULL && app_state.order_capacity == 0);
    CHECK(app_state.likes == NULL && app_state.likes_capacity == 0 && app_state.likes_count == 0);
    CHECK(app_state.play_order == NULL && app_state.play_order_capacity == 0 &&
          app_state.play_order_count == 0);
    CHECK(app_state.playlists == NULL && app_state.playlist_capacity == 0);

    for (unsigned i = 0; i < TRACK_CACHE_SLOTS; ++i) {
        CHECK(app_state.track_cache[i].items == NULL);
    }

    platform_unlock();
}

static void test_native_text_editing(void) {
    Scene scene;
    scene_init(&scene);
    View view = app_snapshot();
    view.authenticated = true;
    scene_layout(&scene, &view, 1280, 800);
    scene.focus = HIT_SEARCH;

    /* Basic left/right, mid-string insert/backspace/delete, home/end in search. */
    CHECK(scene_input(&scene, "hello world", 11));
    CHECK(scene.search_input.cursor == 11 && scene.search_input.anchor == 11);
    CHECK(scene_key(&scene, &view, KEY_LEFT, false));
    CHECK(scene_key(&scene, &view, KEY_LEFT, false));
    CHECK(scene.search_input.cursor == 9);
    CHECK(scene_input(&scene, "X", 1));
    CHECK(!strcmp(scene.query, "hello worXld") && scene.search_input.cursor == 10);
    CHECK(scene_key(&scene, &view, KEY_DELETE, false));
    CHECK(!strcmp(scene.query, "hello worXd"));
    CHECK(scene_key(&scene, &view, KEY_BACKSPACE, false));
    CHECK(!strcmp(scene.query, "hello word") && scene.search_input.cursor == 9);
    CHECK(scene_key(&scene, &view, KEY_HOME, false) && scene.search_input.cursor == 0);
    CHECK(scene_key(&scene, &view, KEY_END, false) && scene.search_input.cursor == 10);

    /* Word motion & deletion with Ctrl+Left / Ctrl+Right / Ctrl+Backspace / Ctrl+Delete. */
    CHECK(scene_key_mods(&scene, &view, KEY_LEFT, false, true));
    CHECK(scene.search_input.cursor == 6);
    CHECK(scene_key_mods(&scene, &view, KEY_LEFT, false, true));
    CHECK(scene.search_input.cursor == 0);
    CHECK(scene_key_mods(&scene, &view, KEY_RIGHT, false, true));
    CHECK(scene.search_input.cursor == 6);
    CHECK(scene_key(&scene, &view, KEY_END, false));
    CHECK(scene_key_mods(&scene, &view, KEY_BACKSPACE, false, true));
    CHECK(!strcmp(scene.query, "hello "));
    CHECK(scene_key(&scene, &view, KEY_HOME, false));
    CHECK(scene_key_mods(&scene, &view, KEY_DELETE, false, true));
    CHECK(!scene.query[0] && scene.search_input.cursor == 0);

    /* Multi-byte UTF-8 navigation and Ctrl+A selection / copy / cut via UiController. */
    platform_lock();
    app_state.view.authenticated = true;
    platform_unlock();

    UiController controller;
    ui_controller_init(&controller, 1000);
    controller.scene.focus = HIT_SEARCH;
    ui_controller_layout(&controller, 1280, 800);
    CHECK(ui_controller_text(&controller, "Björk ♫", strlen("Björk ♫")));
    CHECK(ui_controller_key(&controller, KEY_LEFT, false, 1010));
    CHECK(ui_controller_key(&controller, KEY_LEFT, true, 1020));
    char copied[SEARCH_CAPACITY] = {0};
    CHECK(ui_controller_copy(&controller, copied, sizeof(copied), false) && !strcmp(copied, " "));
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    CHECK(controller.scene.select_all);
    CHECK(ui_controller_key(&controller, KEY_LEFT, false, 1030));
    CHECK(controller.scene.search_input.cursor == 0 && !controller.scene.select_all);
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    CHECK(ui_controller_key(&controller, KEY_RIGHT, false, 1040));
    CHECK(controller.scene.search_input.cursor == strlen("Björk ♫") && !controller.scene.select_all);
    CHECK(ui_controller_shortcut(&controller, 'c', true) == UI_SHORTCUT_COPY_SEARCH);
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    CHECK(ui_controller_copy(&controller, copied, sizeof(copied), false) && !strcmp(copied, "Björk ♫"));
    CHECK(ui_controller_key(&controller, KEY_HOME, false, 1050));
    CHECK(ui_controller_key(&controller, KEY_RIGHT, true, 1060));
    CHECK(ui_controller_copy(&controller, copied, sizeof(copied), true) && !strcmp(copied, "B"));
    CHECK(!strcmp(controller.scene.query, "jörk ♫"));

    /* List filter supports the same arrow keys, selection, and cut/copy shortcuts. */
    controller.scene.focus = HIT_LIST_FILTER;
    CHECK(ui_controller_text(&controller, "one two", 7));
    CHECK(ui_controller_key(&controller, KEY_LEFT, false, 1070));
    CHECK(ui_controller_key(&controller, KEY_DELETE, false, 1080));
    CHECK(!strcmp(controller.scene.list_filter, "one tw"));
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    CHECK(ui_controller_copy(&controller, copied, sizeof(copied), true) && !strcmp(copied, "one tw"));
    CHECK(!controller.scene.list_filter[0]);

    /* Pixel check: Ctrl+A highlights only the text width, never the empty box around it. */
    controller.scene.focus = HIT_SEARCH;
    CHECK(ui_controller_text(&controller, "hi", 2));
    CHECK(ui_controller_shortcut(&controller, 'a', true) == UI_SHORTCUT_HANDLED);
    ui_controller_layout(&controller, 1280, 800);

    Canvas canvas;
#ifdef _WIN32
    WindowsCanvas backend = {0};
    HDC paint = CreateCompatibleDC(NULL);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 1280;
    info.bmiHeader.biHeight = -800;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *data = NULL;
    HBITMAP image = CreateDIBSection(paint, &info, DIB_RGB_COLORS, &data, NULL, 0);
    CHECK(paint && image && data);
    HGDIOBJ previous = SelectObject(paint, image);
    uint32_t *pixels = data;
    unsigned stride = 1280;
    WindowsBuffer buffer = {.dc = paint, .pixels = pixels, .width = 1280, .height = 800};
    windows_canvas_begin(&backend, &buffer, 1.0f, &canvas);
#else
    LinuxCanvas backend = {0};
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1280, 800);
    cairo_t *paint = cairo_create(surface);
    linux_canvas_begin(&backend, paint, &canvas);
    uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
    unsigned stride = (unsigned)cairo_image_surface_get_stride(surface) / 4;
#endif

    scene_draw(&controller.scene, controller.snapshot, &canvas);
#ifdef _WIN32
    GdiFlush();
#else
    cairo_surface_flush(surface);
#endif
    Rect search_rect = controller.scene.search;
    float input_y = search_rect.y + 6.0f;
    float input_h = search_rect.h - 12.0f;
    unsigned sample_y = (unsigned)(input_y + (input_h - 18.0f) * 0.5f + 1.0f);
    unsigned text_x = (unsigned)(search_rect.x + 39.0f);
    unsigned empty_x = (unsigned)(search_rect.x + search_rect.w - 48.0f);
    CHECK((pixels[sample_y * stride + text_x] & 0xffffffu) == 0x305374u);
    CHECK((pixels[sample_y * stride + empty_x] & 0xffffffu) == DARK_TILE);

    /* Empty search with select_all draws no highlight box at all. */
    CHECK(ui_controller_shortcut(&controller, 'u', true) == UI_SHORTCUT_HANDLED);
    controller.scene.select_all = true;
    scene_draw(&controller.scene, controller.snapshot, &canvas);
#ifdef _WIN32
    GdiFlush();
#else
    cairo_surface_flush(surface);
#endif
    CHECK((pixels[sample_y * stride + text_x] & 0xffffffu) == DARK_TILE);

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

    ui_controller_destroy(&controller);
    app_reset_session();
}

int main(void) {
#ifndef _WIN32
    char data_template[] = "/tmp/native-spotify-tests-XXXXXX";
    const char *data_dir = mkdtemp(data_template);
    CHECK(data_dir != NULL);
    setenv("XDG_DATA_HOME", data_dir, 1);
#endif
    CHECK(platform_init());
    app_initialize();
    test_no_session();
    test_loading_placeholders();
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
    test_lyrics_parser();
    test_player_like_and_header_layout();
    test_intro_session_restore_and_player_links();
    test_controller_and_raster();
    test_memory_bounds();
    test_native_text_editing();

#ifndef _WIN32
    test_rendering();
#else
    test_windows_frame_pixels();
    test_native_text_compositor();
#endif

    test_play_pause_burst();
    test_space_toggles_playback();
    test_sidebar_resize_and_collapse();
    test_recent_search_popup();
    test_wheel_settings();
    test_artist_likes();
    test_optimistic_likes();
    test_like_status_before_scan();
    test_playlist_sort_filter_and_play();
    test_devices_panel_and_badge();
    test_app_managed_queue();
    test_playlist_queue_and_cache();
    test_song_radio_and_art_play();
    test_playlist_sorting_and_settings();
    printf("PASS: %u interaction checks (native scene layout, hit testing, input, animation settling, "
           "deferred commands)\n",
           checks);
    return 0;
}
