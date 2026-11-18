#include "artwork.h"
#include "platform.h"

#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_LIBWEBP
#include <webp/decode.h>
#endif

typedef struct {
    struct jpeg_decompress_struct decoder;
    struct jpeg_error_mgr error;
    jmp_buf jump;
    unsigned char *row;
    uint32_t *pixels;
    bool eof;
} JpegImage;

static void jpeg_failure(j_common_ptr decoder) {
    JpegImage *image = decoder->client_data;
    longjmp(image->jump, 1);
}

static void jpeg_quiet(j_common_ptr decoder, int msg_level) {
    if (msg_level < 0 && decoder->err->msg_code == JWRN_JPEG_EOF) {
        JpegImage *image = decoder->client_data;
        image->eof = true;
    }
}

static void jpeg_noop(j_common_ptr decoder) {
    (void)decoder;
}

static bool decode_jpeg(const void *data, size_t size, Image *image) {
    JpegImage *jpeg = calloc(1, sizeof(*jpeg));

    if (!jpeg) {
        return false;
    }

    jpeg->decoder.err = jpeg_std_error(&jpeg->error);
    jpeg->error.error_exit = jpeg_failure;
    jpeg->error.emit_message = jpeg_quiet;
    jpeg->error.output_message = jpeg_noop;
    jpeg->decoder.client_data = jpeg;

    if (setjmp(jpeg->jump)) {
        jpeg_destroy_decompress(&jpeg->decoder);
        free(jpeg->row);
        free(jpeg->pixels);
        free(jpeg);
        return false;
    }

    jpeg_create_decompress(&jpeg->decoder);
    jpeg_mem_src(&jpeg->decoder, data, (unsigned long)size);
    jpeg_read_header(&jpeg->decoder, TRUE);

    if (!image_dimensions_valid(jpeg->decoder.image_width, jpeg->decoder.image_height)) {
        jpeg_destroy_decompress(&jpeg->decoder);
        free(jpeg);
        return false;
    }

    bool cmyk = jpeg->decoder.jpeg_color_space == JCS_CMYK || jpeg->decoder.jpeg_color_space == JCS_YCCK;
    jpeg->decoder.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    jpeg_start_decompress(&jpeg->decoder);
    unsigned channels = (unsigned)jpeg->decoder.output_components;
    unsigned longest = jpeg->decoder.output_width > jpeg->decoder.output_height ? jpeg->decoder.output_width
                                                                                : jpeg->decoder.output_height;
    unsigned width = longest > ARTWORK_SIDE_LIMIT ? jpeg->decoder.output_width * ARTWORK_SIDE_LIMIT / longest
                                                  : jpeg->decoder.output_width;
    unsigned height = longest > ARTWORK_SIDE_LIMIT
                          ? jpeg->decoder.output_height * ARTWORK_SIDE_LIMIT / longest
                          : jpeg->decoder.output_height;
    width = width ? width : 1;
    height = height ? height : 1;
    jpeg->pixels = malloc(width * height * sizeof(*jpeg->pixels));
    jpeg->row = malloc((size_t)jpeg->decoder.output_width * channels);

    if (!jpeg->pixels || !jpeg->row) {
        jpeg_destroy_decompress(&jpeg->decoder);
        free(jpeg->pixels);
        free(jpeg->row);
        free(jpeg);
        return false;
    }

    unsigned target = 0;

    while (jpeg->decoder.output_scanline < jpeg->decoder.output_height) {
        unsigned source = jpeg->decoder.output_scanline;
        jpeg_read_scanlines(&jpeg->decoder, &jpeg->row, 1);

        if (target < height && source == target * jpeg->decoder.output_height / height) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned offset = x * jpeg->decoder.output_width / width * channels;
                uint32_t r = jpeg->row[offset];
                uint32_t g = jpeg->row[offset + 1];
                uint32_t b = jpeg->row[offset + 2];

                if (cmyk && channels >= 4) {
                    uint32_t k = jpeg->row[offset + 3];
                    r = (r * k) / 255u;
                    g = (g * k) / 255u;
                    b = (b * k) / 255u;
                }

                jpeg->pixels[target * width + x] = 0xff000000u | (r << 16) | (g << 8) | b;
            }

            ++target;
        }
    }

    jpeg_finish_decompress(&jpeg->decoder);
    bool ok = !jpeg->eof && target == height;
    jpeg_destroy_decompress(&jpeg->decoder);
    free(jpeg->row);

    if (ok) {
        *image = (Image){width, height, jpeg->pixels};
    }

    if (!ok) {
        free(jpeg->pixels);
    }

    free(jpeg);
    return ok;
}

static bool decode_png(const void *data, size_t size, Image *image) {
    png_image source = {.version = PNG_IMAGE_VERSION};

    if (!png_image_begin_read_from_memory(&source, data, size)) {
        png_image_free(&source);
        return false;
    }

    if (!image_dimensions_valid(source.width, source.height)) {
        png_image_free(&source);
        return false;
    }

    source.format = PNG_FORMAT_RGB;
    unsigned char *pixels = malloc(PNG_IMAGE_SIZE(source));
    png_color background = {0};

    if (!pixels || !png_image_finish_read(&source, &background, pixels, 0, NULL)) {
        free(pixels);
        png_image_free(&source);
        return false;
    }

    unsigned longest = source.width > source.height ? source.width : source.height;
    unsigned width =
        longest > ARTWORK_SIDE_LIMIT ? source.width * ARTWORK_SIDE_LIMIT / longest : source.width;
    unsigned height =
        longest > ARTWORK_SIDE_LIMIT ? source.height * ARTWORK_SIDE_LIMIT / longest : source.height;
    width = width ? width : 1;
    height = height ? height : 1;
    uint32_t *output = malloc(width * height * sizeof(*output));

    if (output) {
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned offset =
                    ((y * source.height / height) * source.width + x * source.width / width) * 3;
                output[y * width + x] = 0xff000000u | (uint32_t)pixels[offset] << 16 |
                                        (uint32_t)pixels[offset + 1] << 8 | pixels[offset + 2];
            }
        }

        *image = (Image){width, height, output};
    }

    free(pixels);
    png_image_free(&source);
    return output != NULL;
}

#ifdef HAVE_LIBWEBP
static bool decode_webp(const void *data, size_t size, Image *image) {
    int src_width = 0;
    int src_height = 0;

    if (!WebPGetInfo(data, size, &src_width, &src_height) || src_width <= 0 || src_height <= 0 ||
        !image_dimensions_valid((unsigned)src_width, (unsigned)src_height)) {
        return false;
    }

    uint8_t *rgba = WebPDecodeRGBA(data, size, &src_width, &src_height);

    if (!rgba) {
        return false;
    }

    unsigned sw = (unsigned)src_width;
    unsigned sh = (unsigned)src_height;
    unsigned longest = sw > sh ? sw : sh;
    unsigned width = longest > ARTWORK_SIDE_LIMIT ? sw * ARTWORK_SIDE_LIMIT / longest : sw;
    unsigned height = longest > ARTWORK_SIDE_LIMIT ? sh * ARTWORK_SIDE_LIMIT / longest : sh;
    width = width ? width : 1;
    height = height ? height : 1;
    uint32_t *output = malloc(width * height * sizeof(*output));

    if (output) {
        for (unsigned y = 0; y < height; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                unsigned offset = ((y * sh / height) * sw + x * sw / width) * 4;
                uint32_t a = rgba[offset + 3];
                uint32_t r = rgba[offset] * a / 255u;
                uint32_t g = rgba[offset + 1] * a / 255u;
                uint32_t b = rgba[offset + 2] * a / 255u;
                output[y * width + x] = 0xff000000u | (r << 16) | (g << 8) | b;
            }
        }

        *image = (Image){width, height, output};
    }

    WebPFree(rgba);
    return output != NULL;
}
#endif

bool platform_image_decode(const void *data, size_t size, Image *image) {
    *image = (Image){0};

    if (!data || size < 8 || size > HTTP_BODY_LIMIT) {
        return false;
    }

    if (!png_sig_cmp(data, 0, 8)) {
        return decode_png(data, size, image);
    }

    const unsigned char *bytes = data;

    if (bytes[0] == 0xff && bytes[1] == 0xd8) {
        return decode_jpeg(data, size, image);
    }

#ifdef HAVE_LIBWEBP
    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4)) {
        return decode_webp(data, size, image);
    }
#endif

    return false;
}
