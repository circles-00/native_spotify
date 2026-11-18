#define COBJMACROS
#include "artwork.h"
#include "platform.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>

bool platform_image_decode(const void *data, size_t size, Image *image) {
    *image = (Image){0};

    if (!data || size < 8 || size > HTTP_BODY_LIMIT) {
        return false;
    }

    const unsigned char *bytes = data;
    bool jpeg = bytes[0] == 0xff && bytes[1] == 0xd8;
    bool png = !memcmp(data, "\x89PNG\r\n\x1a\n", 8);
    bool webp = size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(bytes + 8, "WEBP", 4);

    if (!jpeg && !png && !webp) {
        return false;
    }

    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return false;
    }

    IWICImagingFactory *factory = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *converter = NULL;
    uint32_t *pixels = NULL;
    bool ok = false;
    UINT width = 0, height = 0;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&factory)) ||
        FAILED(IWICImagingFactory_CreateStream(factory, &stream)) ||
        FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)size)) ||
        FAILED(IWICImagingFactory_CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) ||
        FAILED(IWICBitmapFrameDecode_GetSize(frame, &width, &height)) ||
        !image_dimensions_valid(width, height)) {
        goto done;
    }

    unsigned longest = width > height ? width : height;
    width = longest > ARTWORK_SIDE_LIMIT ? width * ARTWORK_SIDE_LIMIT / longest : width;
    height = longest > ARTWORK_SIDE_LIMIT ? height * ARTWORK_SIDE_LIMIT / longest : height;
    width = width ? width : 1;
    height = height ? height : 1;

    if (FAILED(IWICImagingFactory_CreateBitmapScaler(factory, &scaler)) ||
        FAILED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)frame, width, height,
                                           WICBitmapInterpolationModeFant)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(factory, &converter)) ||
        FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)scaler,
                                              &GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, NULL, 0,
                                              WICBitmapPaletteTypeCustom))) {
        goto done;
    }

    pixels = malloc(width * height * sizeof(*pixels));

    if (!pixels || FAILED(IWICFormatConverter_CopyPixels(converter, NULL, width * 4, width * height * 4,
                                                         (BYTE *)pixels))) {
        goto done;
    }

    for (unsigned i = 0; i < width * height; ++i) {
        pixels[i] |= 0xff000000u;
    }

    *image = (Image){width, height, pixels};
    ok = true;

done:
    if (!ok) {
        free(pixels);
    }

    if (converter) {
        IWICFormatConverter_Release(converter);
    }
    if (scaler) {
        IWICBitmapScaler_Release(scaler);
    }
    if (frame) {
        IWICBitmapFrameDecode_Release(frame);
    }
    if (decoder) {
        IWICBitmapDecoder_Release(decoder);
    }
    if (stream) {
        IWICStream_Release(stream);
    }
    if (factory) {
        IWICImagingFactory_Release(factory);
    }

    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }

    return ok;
}
