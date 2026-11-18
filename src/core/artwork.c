#include "artwork.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

bool artwork_url_valid(const char *url) {
    static const char *prefixes[] = {"https://i.scdn.co/image/", "https://mosaic.scdn.co/"};

    if (!url || strlen(url) >= ARTWORK_URL_CAPACITY) {
        return false;
    }

    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t prefix = strlen(prefixes[i]);

        if (strncmp(url, prefixes[i], prefix) || !url[prefix]) {
            continue;
        }

        for (const char *p = url + prefix; *p; ++p) {
            if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') &&
                *p != '-' && *p != '_' && *p != '/') {
                return false;
            }
        }

        return true;
    }

    return false;
}

void artwork_url_parse(yyjson_val *images, char out[ARTWORK_URL_CAPACITY]) {
    out[0] = 0;
    uint64_t best = UINT64_MAX;
    size_t index, count;
    yyjson_val *value;

    yyjson_arr_foreach(images, index, count, value) {
        yyjson_val *url = yyjson_obj_get(value, "url");

        if (!yyjson_is_str(url) || strlen(yyjson_get_str(url)) != yyjson_get_len(url) ||
            !artwork_url_valid(yyjson_get_str(url))) {
            continue;
        }

        yyjson_val *width = yyjson_obj_get(value, "width");
        uint64_t side = yyjson_is_uint(width) ? yyjson_get_uint(width) : 0;
        uint64_t score = side >= 300 ? side - 300 : 10300 - side;

        if (score < best) {
            best = score;
            memcpy(out, yyjson_get_str(url), yyjson_get_len(url) + 1);
        }
    }
}

bool image_dimensions_valid(unsigned width, unsigned height) {
    return width > 0 && height > 0 && width <= ARTWORK_SOURCE_LIMIT && height <= ARTWORK_SOURCE_LIMIT;
}

bool artwork_fetch(const char *url, Image *image, unsigned *retry_after) {
    *image = (Image){0};

    if (retry_after) {
        *retry_after = 0;
    }

    if (!artwork_url_valid(url)) {
        return false;
    }

    HttpResponse response = {0};
    bool fetched = platform_http("GET", url, NULL, NULL, false, &response);

    if (retry_after) {
        *retry_after = response.retry_after;
    }

    bool ok = fetched && response.status == 200 && platform_image_decode(response.body, response.size, image);
    free(response.body);

    if (!ok) {
        free(image->pixels);
        *image = (Image){0};
    }

    return ok;
}
