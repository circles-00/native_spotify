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
    CHECK(!strcmp(url, "https://api.spotify.com/v1/"
                       "search?type=track,artist,album&q=Bj%C3%B6rk%20%26%20year%3A1997&limit=10&offset=0"));
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
    BrowseRequest request = {.kind = BROWSE_SEARCH, .query = "query 0"};
    BrowsePage page = {.count = 1, .limit = BROWSE_PAGE_SIZE, .total = 1};
    snprintf(page.items[0].title, sizeof(page.items[0].title), "Cached fixture track");
    BrowsePage cached;
    CHECK(!browse_cache_get(&cache, &request, 1000, &cached));
    browse_cache_store(&cache, &request, &page, 1000);
    CHECK(browse_cache_get(&cache, &request, 1001, &cached));
    CHECK(cached.count == 1 && !strcmp(cached.items[0].title, "Cached fixture track"));
    CHECK(!browse_cache_get(&cache, &request, 999, &cached));
    CHECK(!browse_cache_get(&cache, &request, 1000 + BROWSE_CACHE_TTL_MS, &cached));

    for (unsigned i = 0; i < BROWSE_CACHE_LIMIT; ++i) {
        snprintf(request.query, sizeof(request.query), "query %u", i);
        browse_cache_store(&cache, &request, &page, 1000);
    }

    snprintf(request.query, sizeof(request.query), "query 0");
    CHECK(browse_cache_get(&cache, &request, 1002, &cached));
    snprintf(request.query, sizeof(request.query), "new query");
    browse_cache_store(&cache, &request, &page, 1002);
    snprintf(request.query, sizeof(request.query), "query 0");
    CHECK(browse_cache_get(&cache, &request, 1003, &cached));
    snprintf(request.query, sizeof(request.query), "query 1");
    CHECK(!browse_cache_get(&cache, &request, 1003, &cached));
    request.kind = BROWSE_SAVED_TRACKS;
    CHECK(!browse_cache_get(&cache, &request, 1003, &cached));
    browse_cache_store(&cache, &request, &page, 1003);
    request.offset = 10;
    CHECK(!browse_cache_get(&cache, &request, 1003, &cached));
    request.offset = 0;
    page.count = BROWSE_PAGE_SIZE + 1;
    browse_cache_store(&cache, &request, &page, 1004);
    CHECK(browse_cache_get(&cache, &request, 1005, &cached) && cached.count == 1);
    memset(&cache, 0, sizeof(cache));
    CHECK(!browse_cache_get(&cache, &request, 1005, &cached));
}

static void test_search_sections(void) {
    BrowseRequest request = {.kind = BROWSE_SEARCH, .query = "label:stereofox"};
    BrowsePage page;
    CHECK(browse_parse("{\"tracks\":{" PAGING ",\"items\":[" TRACK "]},"
                       "\"artists\":{\"items\":[{\"type\":\"artist\",\"id\":\"" ID
                       "\",\"name\":\"Artist\"}]},"
                       "\"albums\":{\"items\":[{\"type\":\"album\",\"id\":\"" ID "\",\"name\":\"Album\"}]}}",
                       &request, &page));
    CHECK(page.count == 1 && page.artist_count == 1 && page.album_count == 1);
    CHECK(page.artists[0].kind == ITEM_ARTIST && page.albums[0].kind == ITEM_ALBUM);
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
    request.filter = (SearchFilter)99;
    CHECK(!browse_url(&request, url, sizeof(url)));
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

int main(void) {
    test_urls();
    test_parsing();
    test_navigation();
    test_input();
    test_cache();
    test_search_sections();
    test_collection_browsing_and_history();

    printf("PASS: %u browse checks (URLs, pages, UTF-8, selection, pagination, drill-down, playback body)\n",
           checks);
    return 0;
}
