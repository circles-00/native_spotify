#include "artwork.h"
#include "catalog.h"
#include "core.h"
#include "platform.h"

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

static void test_url_selection(void) {
    CHECK(!artwork_url_valid(NULL));
    CHECK(!artwork_url_valid(""));
    CHECK(artwork_url_valid("https://i.scdn.co/image/ab0123"));
    CHECK(artwork_url_valid("https://mosaic.scdn.co/640/abc/def"));
    CHECK(!artwork_url_valid("http://i.scdn.co/image/abc"));
    CHECK(!artwork_url_valid("https://i.scdn.co.evil.test/image/abc"));
    CHECK(!artwork_url_valid("https://i.scdn.co@127.0.0.1/image/abc"));
    CHECK(!artwork_url_valid("https://i.scdn.co/image/../secret"));
    CHECK(!artwork_url_valid("https://i.scdn.co/image/abc?url=localhost"));
    CHECK(!artwork_url_valid("https://i.scdn.co/image/abc\r\nAuthorization: x"));
    CHECK(!artwork_url_valid("https://i.scdn.co/image/"));

    const char *json = "[{\"url\":\"https://i.scdn.co/image/large\",\"width\":640},"
                       "{\"url\":\"https://i.scdn.co/image/small\",\"width\":64},"
                       "{\"url\":\"https://i.scdn.co/image/medium\",\"width\":300}]";
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    CHECK(doc != NULL);
    char url[ARTWORK_URL_CAPACITY];
    artwork_url_parse(yyjson_doc_get_root(doc), url);
    CHECK(!strcmp(url, "https://i.scdn.co/image/medium"));
    yyjson_doc_free(doc);

    artwork_url_parse(NULL, url);
    CHECK(!url[0]);

    const char *item = "{\"id\":\"0123456789abcdefghijkl\",\"name\":\"Example\",\"type\":\"track\","
                       "\"album\":{\"images\":[{\"url\":\"https://i.scdn.co/image/track\",\"width\":300}]}}";
    doc = yyjson_read(item, strlen(item), 0);
    CatalogItem parsed;
    CHECK(catalog_item_parse(yyjson_doc_get_root(doc), ITEM_TRACK, &parsed));
    CHECK(!strcmp(parsed.artwork, "https://i.scdn.co/image/track"));
    yyjson_doc_free(doc);
}

static void test_image(const char *hex, bool exact) {
    unsigned char bytes[1024];
    size_t size = strlen(hex) / 2;
    CHECK(size <= sizeof(bytes));

    for (size_t i = 0; i < size; ++i) {
        unsigned byte;
        CHECK(sscanf(hex + i * 2, "%2x", &byte) == 1);
        bytes[i] = (unsigned char)byte;
    }

    Image image;
    CHECK(platform_image_decode(bytes, size, &image));
    CHECK(image.width == 2 && image.height == 1 && image.pixels);
    unsigned red = (image.pixels[0] >> 16) & 255;
    unsigned green = (image.pixels[0] >> 8) & 255;
    unsigned blue = image.pixels[0] & 255;
    CHECK(exact ? red == 25 && green == 175 && blue == 90
                : red >= 23 && red <= 27 && green >= 173 && green <= 177 && blue >= 88 && blue <= 92);
    free(image.pixels);
    CHECK(!platform_image_decode(bytes, size / 2, &image));
    CHECK(!image.pixels);
}

int main(void) {
    CHECK(platform_init());
    test_url_selection();
    CHECK(image_dimensions_valid(1, 1));
    CHECK(image_dimensions_valid(2048, 2048));
    CHECK(!image_dimensions_valid(0, 1));
    CHECK(!image_dimensions_valid(1, 0));
    CHECK(!image_dimensions_valid(2049, 1));
    CHECK(!image_dimensions_valid(1, UINT32_MAX));

    Image image;
    unsigned retry_after = 99;
    CHECK(!artwork_fetch(NULL, &image, &retry_after));
    CHECK(retry_after == 0 && !image.pixels);
    CHECK(!artwork_fetch("http://i.scdn.co/image/abc", &image, &retry_after));
    CHECK(!image.pixels);
    CHECK(!platform_image_decode(NULL, 100, &image));
    CHECK(!platform_image_decode("not an image", 12, &image));
    CHECK(!platform_image_decode("x", HTTP_BODY_LIMIT + 1, &image));

    test_image("89504e470d0a1a0a0000000d49484452000000020000000108020000007b40e8dd0000000f49444154789c63945c1"
               "fc5c0c0000005770124477d3e990000000049454e44ae426082",
               true);
    test_image(
        "ffd8ffe000104a46494600010100000100010000ffdb00430002010101010102010101020202020204030202020205040403"
        "04060506060605060606070908060709070606080b08090a0a0a0a0a06080b0c0b0a0c090a0a0affdb004301020202020202"
        "050303050a0706070a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a"
        "0a0a0a0a0a0a0a0affc00011080001000203012200021101031101ffc4001500010100000000000000000000000000000006"
        "ffc40014100100000000000000000000000000000000ffc4001501010100000000000000000000000000000709ffc4001411"
        "0100000000000000000000000000000000ffda000c03010002110311003f009f00769defffd9",
        false);
    printf("PASS: %u artwork checks (CDN allowlist, metadata, real PNG/JPEG decoding, corrupt input and "
           "dimensions)\n",
           checks);
    return 0;
}
