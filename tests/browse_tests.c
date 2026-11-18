#include "browse.h"
#include "browse_cache.h"
#include "catalog.h"
#include "collection.h"

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

#define ID "0123456789abcdefghijkl"
#define TRACK                                                                                                \
    "{\"id\":\"" ID "\",\"name\":\"Björk – Jóga\",\"type\":\"track\",\"artists\":[{\"name\":\"Björk\"}]}"
#define PAGING                                                                                               \
    "\"offset\":0,\"limit\":10,\"total\":21,\"next\":\"https://untrusted.example/never-follow-this\""

static void test_urls(void) {
    BrowseRequest request = {.kind = BROWSE_SEARCH, .query = "Björk & year:1997"};
    char url[1200];

    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(!strcmp(
        url, "https://api.spotify.com/v1/"
             "search?type=track,artist,album,playlist&q=Bj%C3%B6rk%20%26%20year%3A1997&limit=10&offset=0"));
    CHECK(!browse_url(&request, url, 8));

    request.offset = 1001;
    CHECK(!browse_url(&request, url, sizeof(url)));

    request = (BrowseRequest){.kind = BROWSE_SAVED_TRACKS, .offset = 20};
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/me/tracks?limit=10&offset=20") != NULL);

    request.kind = BROWSE_SAVED_ALBUMS;
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/me/albums?") != NULL);

    request.kind = BROWSE_PLAYLISTS;
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/me/playlists?") != NULL);

    request = (BrowseRequest){.kind = BROWSE_PLAYLIST_TRACKS, .id = ID};
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/playlists/" ID "/items?") != NULL);

    request.kind = BROWSE_ALBUM_TRACKS;
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/albums/" ID "/tracks?") != NULL);

    request.id[0] = '/';
    CHECK(!browse_url(&request, url, sizeof(url)));
    CHECK(!spotify_id_valid("../../../../../../../../"));
    CHECK(!spotify_id_valid(NULL));

    request.kind = (BrowseKind)99;
    CHECK(!browse_url(&request, url, sizeof(url)));
}

static void test_parsing(void) {
    BrowseRequest request = {.kind = BROWSE_SEARCH};
    BrowsePage page = {0};

    CHECK(browse_parse("{\"tracks\":{" PAGING ",\"items\":[" TRACK "]}}", &request, &page));
    CHECK(page.count == 1 && page.total == 21 && page.limit == 10 && page.has_next);
    CHECK(!strcmp(page.items[0].title, "Björk – Jóga"));
    CHECK(!strcmp(page.items[0].detail, "Björk"));
    CHECK(page.items[0].playable);

    request.kind = BROWSE_SAVED_TRACKS;
    CHECK(browse_parse("{" PAGING ",\"items\":[null,{\"track\":null},{\"track\":" TRACK "}]}", &request,
                       &page));
    CHECK(page.count == 1 && page.items[0].kind == ITEM_TRACK);

    request.kind = BROWSE_SAVED_ALBUMS;
    CHECK(browse_parse("{" PAGING ",\"items\":[{\"album\":{\"id\":\"" ID
                       "\",\"name\":\"Album\",\"type\":\"album\"}}]}",
                       &request, &page));
    CHECK(page.count == 1 && page.items[0].kind == ITEM_ALBUM);

    request.kind = BROWSE_PLAYLISTS;
    CHECK(browse_parse("{" PAGING ",\"items\":[{\"id\":\"" ID
                       "\",\"name\":\"Playlist\",\"owner\":{\"display_name\":\"Owner\"}}]}",
                       &request, &page));
    CHECK(page.items[0].kind == ITEM_PLAYLIST);
    CHECK(!strcmp(page.items[0].detail, "Owner"));

    request.kind = BROWSE_PLAYLIST_TRACKS;
    CHECK(browse_parse("{" PAGING ",\"items\":[{\"item\":" TRACK "},{\"track\":" TRACK
                       "},{\"item\":null,\"track\":" TRACK "}]}",
                       &request, &page));
    CHECK(page.count == 2);

    request.kind = BROWSE_ALBUM_TRACKS;
    CHECK(browse_parse("{" PAGING ",\"items\":[" TRACK ",{\"id\":\"" ID
                       "\",\"name\":\"Unavailable\",\"is_playable\":false},{\"id\":\"" ID
                       "\",\"name\":\"Local\",\"is_local\":true},{\"id\":\"" ID
                       "\",\"name\":\"Podcast\",\"type\":\"episode\"}]}",
                       &request, &page));
    CHECK(page.count == 2 && !page.items[1].playable);

    BrowsePage before = page;
    const char *invalid[] = {"not json",
                             "null",
                             "{}",
                             "{\"items\":{}}",
                             "{\"items\":[],\"offset\":0,\"limit\":0,\"total\":0,\"next\":null}",
                             "{\"items\":[],\"offset\":0,\"limit\":11,\"total\":0,\"next\":null}",
                             "{\"items\":[],\"offset\":1,\"limit\":10,\"total\":0,\"next\":null}",
                             "{\"items\":[],\"offset\":0,\"limit\":10,\"total\":4294967296,\"next\":null}",
                             "{\"items\":[],\"offset\":0,\"limit\":10,\"total\":0,\"next\":false}"};

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!browse_parse(invalid[i], &request, &page));
        CHECK(!memcmp(&page, &before, sizeof(page)));
    }

    CHECK(
        browse_parse("{\"items\":[],\"offset\":0,\"limit\":10,\"total\":0,\"next\":null}", &request, &page));
    CHECK(!page.count && !page.has_next);

    char oversized[512] = "{" PAGING ",\"items\":[null";

    for (unsigned i = 0; i < BROWSE_PAGE_SIZE; ++i) {
        strcat(oversized, ",null");
    }
    strcat(oversized, "]}");

    CHECK(!browse_parse(oversized, &request, &page));
}

static void test_navigation(void) {
    Browser browser;
    browser_reset(&browser);

    BrowseRequest next;
    CHECK(!browser_selected(&browser));
    CHECK(!browser_page_request(&browser, 1, &next));
    CHECK(!browser_select(&browser, 0));

    BrowseRequest request = {.kind = BROWSE_SEARCH, .query = "original query"};
    BrowsePage page;
    CHECK(browse_parse("{\"tracks\":{" PAGING ",\"items\":[" TRACK "]}}", &request, &page));

    browser_commit(&browser, &request, &page);
    CHECK(browser.loaded && browser.selected == -1);
    CHECK(browser_select(&browser, 0));
    CHECK(browser_selected(&browser) != NULL);
    CHECK(!browser_select(&browser, 1));
    CHECK(browser.selected == 0);
    CHECK(!browser_child_request(&browser, &next));

    CHECK(browser_page_request(&browser, 1, &next));
    CHECK(next.offset == 10 && !strcmp(next.query, "original query"));

    char safe_url[1200];
    CHECK(browse_url(&next, safe_url, sizeof(safe_url)));
    CHECK(!strncmp(safe_url, "https://api.spotify.com/v1/search?", 34));
    CHECK(!strstr(safe_url, "untrusted.example"));
    CHECK(!browser_page_request(&browser, -1, &next));
    CHECK(!browser_page_request(&browser, 0, &next));

    request.offset = 10;
    page.offset = 10;
    browser_commit(&browser, &request, &page);

    CHECK(browser.selected == -1);
    CHECK(browser_page_request(&browser, -1, &next) && next.offset == 0);

    browser.request.offset = 1000;
    CHECK(!browser_page_request(&browser, 1, &next));

    browser.page.items[0].kind = ITEM_ALBUM;
    CHECK(browser_select(&browser, 0));
    CHECK(browser_child_request(&browser, &next));
    CHECK(next.kind == BROWSE_ALBUM_TRACKS && !strcmp(next.id, ID) && !next.offset);

    browser.page.items[0].kind = ITEM_PLAYLIST;
    CHECK(browser_child_request(&browser, &next));
    CHECK(next.kind == BROWSE_PLAYLIST_TRACKS);

    char url[128];
    CHECK(item_link(browser_selected(&browser), url, sizeof(url)));
    CHECK(!strcmp(url, "https://open.spotify.com/playlist/" ID));

    browser.page.items[0].kind = ITEM_TRACK;
    CHECK(item_play_body(browser_selected(&browser), url, sizeof(url)));
    CHECK(!strcmp(url, "{\"uris\":[\"spotify:track:" ID "\"]}"));
    char body[256];
    CHECK(item_context_body(browser_selected(&browser), &next, body, sizeof(body)));
    CHECK(!strcmp(body, "{\"context_uri\":\"spotify:playlist:" ID "\",\"offset\":{\"uri\":\"spotify:track:" ID
                        "\"}}"));
    next.kind = BROWSE_ALBUM_TRACKS;
    CHECK(item_context_body(browser_selected(&browser), &next, body, sizeof(body)));
    CHECK(strstr(body, "spotify:album:" ID) != NULL);
    CHECK(!item_context_body(browser_selected(&browser), &next, body, 16));
    next.id[0] = '"';
    CHECK(!item_context_body(browser_selected(&browser), &next, body, sizeof(body)));
    next.kind = BROWSE_SEARCH;
    CHECK(item_context_body(browser_selected(&browser), &next, body, sizeof(body)));
    CHECK(!strcmp(body, "{\"uris\":[\"spotify:track:" ID "\"]}"));

    browser.page.items[0].playable = false;
    CHECK(!item_play_body(browser_selected(&browser), url, sizeof(url)));

    browser.page.items[0].id[0] = '"';
    browser.page.items[0].playable = true;
    CHECK(!item_link(browser_selected(&browser), url, sizeof(url)));
    CHECK(!item_play_body(browser_selected(&browser), url, sizeof(url)));

    browser_reset(&browser);
    CHECK(!browser.loaded && !browser.page.count && !browser.request.query[0] && !browser_selected(&browser));
}

static void test_input(void) {
    char query[SEARCH_CAPACITY] = {0};

    CHECK(search_append(query, "Björk ♫", strlen("Björk ♫")));
    search_backspace(query);
    CHECK(!strcmp(query, "Björk "));

    CHECK(!search_append(query, "\n", 1));
    CHECK(!search_append(query, "\xc0\xaf", 2));
    CHECK(!search_append(query, "\xed\xa0\x80", 3));
    CHECK(!search_append(query, "\xf4\x90\x80\x80", 4));
    CHECK(!search_append(query, "\xe2\x99", 2));
    CHECK(!strcmp(query, "Björk "));

    memset(query, 'x', sizeof(query) - 1);
    query[sizeof(query) - 1] = 0;
    CHECK(!search_append(query, "x", 1));

    query[0] = 0;
    search_backspace(query);
    CHECK(!query[0]);
}

static void test_cache(void) {
    BrowseCache cache = {0};
    BrowseRequest library = {.kind = BROWSE_SAVED_TRACKS};
    BrowsePage page = {.count = 1, .limit = BROWSE_PAGE_SIZE, .total = 1};
    snprintf(page.items[0].title, sizeof(page.items[0].title), "Cached fixture track");
    BrowsePage cached;
    CHECK(!browse_cache_get(&cache, &library, 1000, &cached));
    browse_cache_store(&cache, &library, &page, 1000);
    CHECK(browse_cache_get(&cache, &library, 1001, &cached));
    CHECK(cached.count == 1 && !strcmp(cached.items[0].title, "Cached fixture track"));
    CHECK(!browse_cache_get(&cache, &library, 999, &cached));
    CHECK(!browse_cache_get(&cache, &library, 1000 + BROWSE_CACHE_TTL_MS, &cached));

    BrowseRequest request = {.kind = BROWSE_ALBUM_TRACKS};

    for (unsigned i = 0; i < BROWSE_CACHE_LIMIT - BROWSE_CACHE_LIBRARY_SLOTS; ++i) {
        snprintf(request.id, sizeof(request.id), "%022u", i);
        browse_cache_store(&cache, &request, &page, 1000);
    }

    snprintf(request.id, sizeof(request.id), "%022u", 0u);
    CHECK(browse_cache_get(&cache, &request, 1002, &cached));
    snprintf(request.id, sizeof(request.id), "%022u", 99u);
    browse_cache_store(&cache, &request, &page, 1002);
    snprintf(request.id, sizeof(request.id), "%022u", 0u);
    CHECK(browse_cache_get(&cache, &request, 1003, &cached));
    snprintf(request.id, sizeof(request.id), "%022u", 1u);
    CHECK(!browse_cache_get(&cache, &request, 1003, &cached));
    CHECK(browse_cache_get(&cache, &library, 1003, &cached));

    BrowseRequest uncached[] = {{.kind = BROWSE_SEARCH, .query = "one-off search"},
                                {.kind = BROWSE_QUEUE},
                                {.kind = BROWSE_SAVED_TRACKS, .offset = 10},
                                {.kind = BROWSE_FOLLOWED_ARTISTS, .after = ID}};

    for (unsigned i = 0; i < sizeof(uncached) / sizeof(*uncached); ++i) {
        page.offset = uncached[i].offset;
        browse_cache_store(&cache, &uncached[i], &page, 1003);
        CHECK(!browse_cache_get(&cache, &uncached[i], 1004, &cached));
    }

    page.offset = 0;
    page.count = BROWSE_PAGE_SIZE + 1;
    browse_cache_store(&cache, &library, &page, 1004);
    CHECK(browse_cache_get(&cache, &library, 1005, &cached) && cached.count == 1);
    browse_cache_invalidate(&cache, &library);
    CHECK(!browse_cache_get(&cache, &library, 1005, &cached));
}

static void test_search_sections(void) {
    BrowseRequest request = {.kind = BROWSE_SEARCH, .query = "label:stereofox"};
    BrowsePage page;
    CHECK(browse_parse(
        "{\"tracks\":{" PAGING ",\"items\":[" TRACK "]},"
        "\"artists\":{\"items\":[{\"type\":\"artist\",\"id\":\"" ID "\",\"name\":\"Artist\"}]},"
        "\"albums\":{\"items\":[{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"Album\"}]},"
        "\"playlists\":{\"items\":[null,{\"type\":\"playlist\",\"id\":\"" ID
        "\",\"name\":\"Playlist\",\"owner\":{\"id\":\"spotify\",\"display_name\":\"Spotify\"}}]}}",
        &request, &page));
    CHECK(page.count == 1 && page.artist_count == 1 && page.album_count == 1 && page.playlist_count == 1);
    CHECK(page.artists[0].kind == ITEM_ARTIST && page.albums[0].kind == ITEM_ALBUM &&
          page.playlists[0].kind == ITEM_PLAYLIST);
    CHECK(!strcmp(page.playlists[0].owner_id, "spotify") && !strcmp(page.playlists[0].detail, "Spotify"));
    char body[256];
    CHECK(item_context_body(&page.artists[0], &request, body, sizeof(body)));
    CHECK(!strcmp(body, "{\"context_uri\":\"spotify:artist:" ID "\"}"));

    BrowseCache cache = {0};
    browse_cache_store(&cache, &request, &page, 100);
    request.filter = SEARCH_ARTISTS;
    CHECK(!browse_cache_get(&cache, &request, 200, &page));
    char url[1200];
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "type=artist&q=label%3Astereofox"));
    CHECK(browse_parse("{\"artists\":{" PAGING ",\"items\":[{\"type\":\"artist\",\"id\":\"" ID
                       "\",\"name\":\"Artist\"}]}}",
                       &request, &page));
    CHECK(page.count == 1 && page.items[0].kind == ITEM_ARTIST);
    request.filter = SEARCH_ALBUMS;
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "type=album&"));
    request.filter = SEARCH_SONGS;
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "type=track&"));
    request.filter = SEARCH_PLAYLISTS;
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "type=playlist&"));
    CHECK(browse_parse("{\"playlists\":{" PAGING ",\"items\":[null,{\"type\":\"playlist\",\"id\":\"" ID
                       "\",\"name\":\"Chill Mix\",\"owner\":{\"display_name\":\"Spotify\"}}]}}",
                       &request, &page));
    CHECK(page.count == 1 && page.items[0].kind == ITEM_PLAYLIST &&
          !strcmp(page.items[0].title, "Chill Mix"));
    request.filter = (SearchFilter)99;
    CHECK(!browse_url(&request, url, sizeof(url)));
}

static void test_search_top_context(void) {
    CatalogItem tracks[3] = {
        {.kind = ITEM_TRACK,
         .id = "0123456789abcdefghijk1",
         .title = "Army of Me",
         .detail = "Björk",
         .album_id = "0123456789abcdefghija1",
         .album_title = "Post",
         .popularity = 65},
        {.kind = ITEM_TRACK,
         .id = "0123456789abcdefghijk2",
         .title = "Jóga",
         .detail = "Björk",
         .album_id = "0123456789abcdefghija2",
         .album_title = "Homogenic",
         .popularity = 64},
        {.kind = ITEM_TRACK,
         .id = "0123456789abcdefghijk3",
         .title = "Bohemian Rhapsody - Remastered 2011",
         .detail = "Queen",
         .album_id = "0123456789abcdefghija3",
         .album_title = "A Night at the Opera",
         .popularity = 88},
    };
    CatalogItem artists[2] = {
        {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijb1", .title = "Björk", .popularity = 72},
        {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijb2", .title = "Bohemian Rhapsody", .popularity = 4},
    };
    CatalogItem albums[2] = {
        {.kind = ITEM_ALBUM,
         .id = "0123456789abcdefghija2",
         .title = "Homogenic",
         .detail = "Björk",
         .album_type = "album"},
        {.kind = ITEM_ALBUM,
         .id = "0123456789abcdefghija4",
         .title = "Jóga",
         .detail = "Björk",
         .album_type = "single"},
    };
    CatalogItem playlists[2] = {
        {.kind = ITEM_PLAYLIST,
         .id = "0123456789abcdefghijp1",
         .title = "lofi beats",
         .detail = "Spotify",
         .owner_id = "spotify"},
        {.kind = ITEM_PLAYLIST,
         .id = "0123456789abcdefghijp2",
         .title = "Björk",
         .detail = "Listener",
         .owner_id = "user123"},
    };

    /* Artist query picks the artist over tracks and user playlists with the same name. */
    const CatalogItem *top = browse_search_top("bjork", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &artists[0]);

    /* Song query picks the song (even with Remastered suffix) over an obscure homonym artist. */
    top = browse_search_top("Bohemian Rhapsody", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &tracks[2]);

    /* Song + artist query picks the track over the artist, even when the track is not index 0. */
    top = browse_search_top("joga bjork", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &tracks[1]);

    /* Single wrapper album with same name as a song loses to the song itself. */
    top = browse_search_top("Jóga", tracks + 1, 1, artists, 1, albums + 1, 1, playlists, 0);
    CHECK(top == &tracks[1]);

    /* Album query with album tracks in context picks the album. */
    top = browse_search_top("Homogenic Björk", tracks, 3, artists, 1, albums, 2, playlists, 2);
    CHECK(top == &albums[0]);

    /* Playlist query picks the playlist over unrelated artists/tracks. */
    top = browse_search_top("lofi beats", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &playlists[0]);

    /* Explicit operators route context cleanly. */
    top = browse_search_top("album:Post", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &albums[0] || top == &tracks[0]);
    top = browse_search_top("artist:Bohemian Rhapsody", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &artists[1]);
    top = browse_search_top("label:stereofox", tracks, 3, artists, 2, albums, 2, playlists, 2);
    CHECK(top == &tracks[0]);

    /* Guest artist match works for featured collaborators. */
    CatalogItem collab = {.kind = ITEM_TRACK,
                          .id = "0123456789abcdefghijk9",
                          .title = "Get Lucky",
                          .detail = "Daft Punk",
                          .guest_artists = "Pharrell Williams Nile Rodgers"};
    CatalogItem daft = {.kind = ITEM_ARTIST, .id = "0123456789abcdefghijb9", .title = "Daft Punk"};
    top = browse_search_top("get lucky pharrell", &collab, 1, &daft, 1, NULL, 0, NULL, 0);
    CHECK(top == &collab);
}

static void test_collection_browsing_and_history(void) {
    CatalogItem artist = {.kind = ITEM_ARTIST, .id = ID, .title = "Artist"};
    BrowseRequest request;
    CHECK(item_browse_request(&artist, &request));
    CHECK(request.kind == BROWSE_ARTIST_ALBUMS && !strcmp(request.title, "Artist"));
    char url[1200];
    CHECK(browse_url(&request, url, sizeof(url)));
    CHECK(strstr(url, "/artists/" ID "/albums?include_groups=album,single&limit=10&offset=0"));
    BrowsePage page;
    CHECK(browse_parse("{" PAGING ",\"items\":[{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"Album\"}]}",
                       &request, &page));
    CHECK(page.count == 1 && page.items[0].kind == ITEM_ALBUM);
    CHECK(item_browse_request(&page.items[0], &request) && request.kind == BROWSE_ALBUM_TRACKS);
    CatalogItem track = {.kind = ITEM_TRACK, .id = ID, .playable = true};
    CHECK(!item_browse_request(&track, &request));
    artist.id[0] = '/';
    CHECK(!item_browse_request(&artist, &request));

    CHECK(recent_albums_parse(
        "{\"items\":[{\"track\":{\"album\":{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"First\"}}},"
        "{\"track\":{\"album\":{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"First\"}}},"
        "{\"track\":null},{\"track\":{\"album\":{\"type\":\"album\",\"id\":\"abcdefghijkl0123456789\","
        "\"name\":\"Second\"}}}]}",
        &page));
    CHECK(page.count == 2 && !strcmp(page.items[0].title, "First") && !strcmp(page.items[1].title, "Second"));
    CHECK(page.items[0].kind == ITEM_ALBUM && !page.has_next);
    CHECK(recent_albums_parse("{\"items\":[]}", &page) && !page.count);
    CHECK(!recent_albums_parse("{\"items\":null}", &page));
    CHECK(!recent_albums_parse("not json", &page));

    // Artist discography filter query URLs
    request = (BrowseRequest){.kind = BROWSE_ARTIST_ALBUMS, .id = ID, .query = "album"};
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "include_groups=album&"));
    snprintf(request.query, sizeof(request.query), "single");
    CHECK(browse_url(&request, url, sizeof(url)) && strstr(url, "include_groups=single&"));
    snprintf(request.query, sizeof(request.query), "invalid");
    CHECK(!browse_url(&request, url, sizeof(url)));

    // Followed artists cursor pagination and profile public playlists filtering
    request = (BrowseRequest){.kind = BROWSE_FOLLOWED_ARTISTS};
    CHECK(browse_url(&request, url, sizeof(url)) &&
          !strcmp(url, "https://api.spotify.com/v1/me/following?type=artist&limit=10"));
    snprintf(request.after, sizeof(request.after), ID);
    CHECK(browse_url(&request, url, sizeof(url)) &&
          strstr(url, "me/following?type=artist&limit=10&after=" ID));
    request.after[0] = '/';
    CHECK(!browse_url(&request, url, sizeof(url)));
    request.after[0] = 0;
    CHECK(browse_parse("{\"artists\":{\"items\":[{\"type\":\"artist\",\"id\":\"" ID
                       "\",\"name\":\"Artist\"}],"
                       "\"limit\":10,\"total\":2,\"next\":\"https://api.spotify.com/next\","
                       "\"cursors\":{\"after\":\"abcdefghijkl0123456789\"}}}",
                       &request, &page));
    CHECK(page.count == 1 && page.has_next && !strcmp(page.next_after, "abcdefghijkl0123456789"));

    request = (BrowseRequest){.kind = BROWSE_PROFILE_PLAYLISTS, .owner_id = "me123"};
    CHECK(browse_parse(
        "{" PAGING ",\"items\":["
        "{\"id\":\"" ID
        "\",\"name\":\"Public mine\",\"public\":true,\"owner\":{\"id\":\"me123\",\"display_name\":\"Me\"}},"
        "{\"id\":\"" ID
        "\",\"name\":\"Private mine\",\"public\":false,\"owner\":{\"id\":\"me123\",\"display_name\":\"Me\"}},"
        "{\"id\":\"" ID "\",\"name\":\"Public "
        "other\",\"public\":true,\"owner\":{\"id\":\"other\",\"display_name\":\"Other\"}}"
        "]}",
        &request, &page));
    CHECK(page.count == 1 && !strcmp(page.items[0].title, "Public mine"));

    // Album and artist collection metadata parsing
    CollectionInfo info = {0};
    CHECK(collection_parse(
        "{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"Abbey Road\",\"album_type\":\"album\","
        "\"release_date\":\"1969-09-26\",\"release_date_precision\":\"day\",\"total_tracks\":2,"
        "\"label\":\"Apple "
        "Records\",\"artists\":[{\"type\":\"artist\",\"id\":\"abcdefghijkl0123456789\",\"name\":\"The "
        "Beatles\"}],"
        "\"copyrights\":[{\"type\":\"C\",\"text\":\"2019 Apple\"},{\"type\":\"P\",\"text\":\"2019 EMI\"}],"
        "\"tracks\":{\"items\":[{\"duration_ms\":259000},{\"duration_ms\":182000}]}}",
        ITEM_ALBUM, &info));
    CHECK(info.loaded && info.total_tracks == 2 && info.duration_complete && info.duration_ms == 441000);
    CHECK(!strcmp(info.artist.title, "The Beatles") && !strcmp(info.label, "Apple Records"));
    CHECK(strstr(info.copyrights, "© 2019 Apple") && strstr(info.copyrights, "℗ 2019 EMI"));
    char label_text[64];
    collection_release_label(&info, label_text, sizeof(label_text));
    CHECK(!strcmp(label_text, "September 26, 1969"));
    collection_duration_label(info.duration_ms, label_text, sizeof(label_text));
    CHECK(!strcmp(label_text, "7 min 21 sec"));
    collection_duration_label(3725000, label_text, sizeof(label_text));
    CHECK(!strcmp(label_text, "1 hr 2 min"));

    CHECK(collection_parse("{\"type\":\"artist\",\"id\":\"" ID
                           "\",\"name\":\"The Beatles\",\"followers\":{\"total\":28500000}}",
                           ITEM_ARTIST, &info));
    CHECK(info.loaded && info.followers_known && info.followers == 28500000);
    CHECK(collection_popular_parse("{\"tracks\":[" TRACK "]}", &info));
    CHECK(info.popular_loaded && info.popular_count == 1 && !strcmp(info.popular[0].title, "Björk – Jóga"));

    ProfileInfo profile = {0};
    CHECK(profile_parse("{\"id\":\"user42\",\"display_name\":\"Listener\",\"followers\":{\"total\":7}}",
                        &profile));
    CHECK(profile.loaded && !strcmp(profile.id, "user42") && !strcmp(profile.name, "Listener") &&
          profile.followers_known && profile.followers == 7);
}

static void test_full_track_lists(void) {
    BrowseRequest playlist = {.kind = BROWSE_PLAYLIST_TRACKS, .id = "0123456789abcdefghijkl", .offset = 50};
    char url[256];
    CHECK(browse_tracks_url(&playlist, url, sizeof(url)));
    CHECK(!strcmp(url, "https://api.spotify.com/v1/playlists/0123456789abcdefghijkl/items?limit=50&offset=50"));

    BrowseRequest liked = {.kind = BROWSE_SAVED_TRACKS};
    CHECK(browse_tracks_url(&liked, url, sizeof(url)) && strstr(url, "me/tracks?limit=50&offset=0"));
    CHECK(!browse_tracks_url(&(BrowseRequest){.kind = BROWSE_SEARCH}, url, sizeof(url)));

    // A local file has no Spotify id: it is skipped but still advances the offset.
    const char *json =
        "{\"items\":[{\"item\":{\"type\":\"track\",\"id\":\"0123456789abcdefghijk1\",\"name\":\"One\","
        "\"artists\":[{\"name\":\"A\"}],\"duration_ms\":1000}},{\"item\":null},"
        "{\"track\":{\"type\":\"track\",\"id\":\"0123456789abcdefghijk2\",\"name\":\"Two\","
        "\"artists\":[{\"name\":\"B\"}],\"duration_ms\":2000}}],"
        "\"total\":120,\"offset\":50,\"limit\":50,\"next\":\"https://api.spotify.com/next\"}";
    TrackPage *page = malloc(sizeof(*page));
    CHECK(page != NULL);
    CHECK(browse_parse_tracks(json, &playlist, page));
    CHECK(page->count == 2 && page->scanned == 3 && page->total == 120 && page->has_next);
    CHECK(!strcmp(page->items[1].title, "Two"));
    ++playlist.offset;
    CHECK(!browse_parse_tracks(json, &playlist, page));
    CHECK(!browse_parse_tracks("{\"items\":{}}", &playlist, page));
    free(page);

    // Liked Songs play as the account's Liked Songs context starting at the chosen song.
    CatalogItem song = {.kind = ITEM_TRACK, .id = "0123456789abcdefghijk1", .playable = true};
    BrowseRequest context = {.kind = BROWSE_SAVED_TRACKS, .owner_id = "listener.name"};
    char body[256];
    CHECK(item_context_body(&song, &context, body, sizeof(body)));
    CHECK(!strcmp(body, "{\"context_uri\":\"spotify:user:listener.name:collection\","
                        "\"offset\":{\"uri\":\"spotify:track:0123456789abcdefghijk1\"}}"));
    CHECK(!browse_user_id_valid("bad\"id") && !browse_user_id_valid(""));
    snprintf(context.owner_id, sizeof(context.owner_id), "bad\"id");
    CHECK(item_context_body(&song, &context, body, sizeof(body)) && !strstr(body, "collection"));
}

int main(void) {
    test_urls();
    test_parsing();
    test_navigation();
    test_input();
    test_cache();
    test_search_sections();
    test_search_top_context();
    test_collection_browsing_and_history();
    test_full_track_lists();

    printf("PASS: %u browse checks (URLs, pages, UTF-8, selection, pagination, drill-down, playback body)\n",
           checks);
    return 0;
}
