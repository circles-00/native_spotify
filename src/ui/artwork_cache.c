#include "artwork.h"
#include "artwork_store.h"
#include "platform.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>

static ArtworkStore store;
static unsigned revision;
static unsigned workers;

static void load_artwork(void *argument) {
    (void)argument;

    for (;;) {
        platform_lock();
        unsigned slot = artwork_store_next(&store);

        if (slot == ARTWORK_CACHE_LIMIT) {
            --workers;
            platform_unlock();
            return;
        }

        store.entries[slot].state = ART_LOADING;
        unsigned generation = revision;
        char url[ARTWORK_URL_CAPACITY];
        memcpy(url, store.entries[slot].url, sizeof(url));
        platform_unlock();

        HttpResponse response = {0};
        Image image = {0};
        bool ok = platform_http("GET", url, NULL, NULL, false, &response) && response.status == 200 &&
                  platform_image_decode(response.body, response.size, &image);
        free(response.body);

        platform_lock();

        if (generation == revision && store.entries[slot].state == ART_LOADING &&
            !strcmp(store.entries[slot].url, url)) {
            store.entries[slot].image = image;
            store.entries[slot].state = ok ? ART_READY : ART_FAILED;
            uint64_t retry = response.retry_after > 30 ? (uint64_t)response.retry_after * 1000 : 30000;
            store.entries[slot].retry_ms = platform_milliseconds() + retry;
            image.pixels = NULL;
        }

        platform_unlock();
        free(image.pixels);
        ui_wake();
    }
}

void artwork_reset(unsigned session_revision) {
    platform_lock();

    if (revision != session_revision) {
        revision = session_revision;

        artwork_store_clear(&store);
    }

    platform_unlock();
}

void artwork_begin_frame(void) {
    platform_lock();
    ++store.frame;
    platform_unlock();
}

void artwork_pin(const char *url) {
    if (!url || !url[0]) {
        return;
    }

    platform_lock();
    artwork_store_pin(&store, url);
    platform_unlock();
}

bool artwork_visit(const char *url, void (*paint)(void *, const Image *), void *context) {
    if (!artwork_url_valid(url)) {
        return false;
    }

    platform_lock();
    ArtworkEntry *entry = artwork_store_request(&store, url, platform_milliseconds());

    if (!entry) {
        platform_unlock();
        return false;
    }

    bool ready = entry->state == ART_READY;
    Image image = ready ? entry->image : (Image){0};

    bool start = workers < 2 && entry->state == ART_PENDING;

    if (start) {
        ++workers;
    }
    platform_unlock();

    if (ready) {
        paint(context, &image);
    }

    if (start && !platform_start_thread(load_artwork, NULL)) {
        platform_lock();
        --workers;
        platform_unlock();
    }

    return ready;
}

static void average_color(void *context, const Image *image) {
    uint64_t red = 0, green = 0, blue = 0;
    unsigned count = 0;
    unsigned step_x = image->width / 8 + 1;
    unsigned step_y = image->height / 8 + 1;

    for (unsigned y = 0; y < image->height; y += step_y) {
        for (unsigned x = 0; x < image->width; x += step_x) {
            uint32_t pixel = image->pixels[y * image->width + x];
            red += pixel >> 16 & 255;
            green += pixel >> 8 & 255;
            blue += pixel & 255;
            ++count;
        }
    }

    uint32_t *color = context;
    *color = (uint32_t)(red / count * 3 / 5) << 16 | (uint32_t)(green / count * 3 / 5) << 8 |
             (uint32_t)(blue / count * 3 / 5);
}

bool artwork_color(const char *url, uint32_t *color) {
    return artwork_visit(url, average_color, color);
}
