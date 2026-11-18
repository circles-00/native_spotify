#include "artwork.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

static bool valid_host(const char *host, size_t len) {
    static const char *const suffixes[] = {".scdn.co", ".spotifycdn.com"};

    if (!len || len >= 128 || host[0] == '.' || host[0] == '-' || host[len - 1] == '.' ||
        host[len - 1] == '-') {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)host[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';

        if (!ok || (ch == '.' && host[i + 1] == '.')) {
            return false;
        }
    }

    for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        size_t slen = strlen(suffixes[i]);

        if (len > slen && !memcmp(host + len - slen, suffixes[i], slen)) {
            return true;
        }
    }

    return false;
}

bool artwork_url_valid(const char *url) {
    static const char https[] = "https://";

    if (!url || strlen(url) >= ARTWORK_URL_CAPACITY || strncmp(url, https, sizeof(https) - 1)) {
        return false;
    }

    const char *host = url + sizeof(https) - 1;
    const char *slash = strchr(host, '/');

    if (!slash || !valid_host(host, (size_t)(slash - host))) {
        return false;
    }

    const char *path = slash + 1;
    size_t plen = strlen(path);

    if (!plen || path[plen - 1] == '/') {
        return false;
    }

    const char *seg = path;

    for (size_t i = 0; i <= plen; ++i) {
        char ch = path[i];

        if (ch == '/' || ch == 0) {
            size_t slen = (size_t)(&path[i] - seg);

            if (!slen || (slen == 1 && seg[0] == '.') || (slen == 2 && seg[0] == '.' && seg[1] == '.')) {
                return false;
            }

            seg = &path[i + 1];
            continue;
        }

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.' || ch == '~')) {
            return false;
        }
    }

    return true;
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

    bool ok = fetched && response.status == 200 && response.size <= ARTWORK_HTTP_LIMIT &&
              platform_image_decode(response.body, response.size, image);
    free(response.body);

    if (!ok) {
        free(image->pixels);
        *image = (Image){0};
    }

    return ok;
}
