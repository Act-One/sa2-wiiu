#include "platform/shared/video/GX2Present.h"

#include <stddef.h>

#if defined(PLATFORM_WIIU)
#include <coreinit/debug.h>

#include "global.h"

#define GX2_PRESENT_LOG(fmt, ...) OSReport("[sa2-wiiu][gx2-present] " fmt "\n", ##__VA_ARGS__)

enum {
    GX2_PRESENT_TRACE_WINDOW = 90,
    GX2_PRESENT_TRACE_INTERVAL = 300,
};

static SDL_Renderer *sRenderer;
static SDL_Texture *sTexture;
static uint32_t *sUploadPixels;
static int sFrameWidth;
static int sFrameHeight;
static SDL_Rect sDestRect;
static int sLastOutputWidth;
static int sLastOutputHeight;
static uint32_t sPresentFrameCounter;

static bool GX2Present_ShouldTrace(void)
{
    return sPresentFrameCounter < GX2_PRESENT_TRACE_WINDOW || (sPresentFrameCounter % GX2_PRESENT_TRACE_INTERVAL) == 0;
}

static uint8_t Expand5To8(uint16_t value) { return (uint8_t)((value << 3) | (value >> 2)); }

static uint32_t Rgb555ToRgba32(uint16_t pixel)
{
    uint8_t r = Expand5To8((pixel >> 0) & 0x1F);
    uint8_t g = Expand5To8((pixel >> 5) & 0x1F);
    uint8_t b = Expand5To8((pixel >> 10) & 0x1F);

    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
}

static void ConvertFrameToRgba32(uint32_t *dst, const uint16_t *src)
{
    size_t pixelCount = (size_t)sFrameWidth * sFrameHeight;

    for (size_t i = 0; i < pixelCount; i++)
        dst[i] = Rgb555ToRgba32(src[i]);
}

static void GX2Present_LogOutputScale(void)
{
    int outputWidth = 0;
    int outputHeight = 0;

    if (SDL_GetRendererOutputSize(sRenderer, &outputWidth, &outputHeight) == 0) {
        GX2_PRESENT_LOG("renderer output=%dx%d source=%dx%d dst=%d,%d %dx%d",
                        outputWidth, outputHeight, sFrameWidth, sFrameHeight, sDestRect.x, sDestRect.y,
                        sDestRect.w, sDestRect.h);
    }
}

static void GX2Present_UpdateDestRect(bool forceLog)
{
    int outputWidth = 0;
    int outputHeight = 0;

    if (SDL_GetRendererOutputSize(sRenderer, &outputWidth, &outputHeight) != 0) {
        outputWidth = sFrameWidth;
        outputHeight = sFrameHeight;
    }

    if (!forceLog && outputWidth == sLastOutputWidth && outputHeight == sLastOutputHeight)
        return;

    sDestRect.x = 0;
    sDestRect.y = 0;
    sDestRect.w = outputWidth;
    sDestRect.h = outputHeight;

    sLastOutputWidth = outputWidth;
    sLastOutputHeight = outputHeight;

    if (forceLog || GX2Present_ShouldTrace())
        GX2Present_LogOutputScale();
}

bool GX2Present_Init(SDL_Window *window, int frameWidth, int frameHeight)
{
    static const Uint32 rendererAttempts[] = {
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
        SDL_RENDERER_PRESENTVSYNC,
        SDL_RENDERER_ACCELERATED,
        0,
    };

    sFrameWidth = frameWidth;
    sFrameHeight = frameHeight;
    sDestRect = (SDL_Rect) { 0, 0, frameWidth, frameHeight };
    sLastOutputWidth = 0;
    sLastOutputHeight = 0;
    sPresentFrameCounter = 0;

    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");

    for (size_t attemptIndex = 0; attemptIndex < ARRAY_COUNT(rendererAttempts); attemptIndex++) {
        sRenderer = SDL_CreateRenderer(window, -1, rendererAttempts[attemptIndex]);
        if (sRenderer != NULL)
            break;
    }

    if (sRenderer == NULL) {
        GX2_PRESENT_LOG("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawColor(sRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sRenderer);

    sTexture = SDL_CreateTexture(sRenderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, frameWidth, frameHeight);
    if (sTexture == NULL) {
        GX2_PRESENT_LOG("SDL_CreateTexture failed: %s", SDL_GetError());
        GX2Present_Shutdown();
        return false;
    }

    SDL_SetTextureBlendMode(sTexture, SDL_BLENDMODE_NONE);

    sUploadPixels = SDL_malloc((size_t)frameWidth * frameHeight * sizeof(*sUploadPixels));
    if (sUploadPixels == NULL) {
        SDL_SetError("Failed to allocate GX2 present upload buffer");
        GX2_PRESENT_LOG("%s", SDL_GetError());
        GX2Present_Shutdown();
        return false;
    }

    {
        SDL_RendererInfo rendererInfo;
        if (SDL_GetRendererInfo(sRenderer, &rendererInfo) == 0) {
            GX2_PRESENT_LOG("renderer=%s flags=0x%08X", rendererInfo.name != NULL ? rendererInfo.name : "<null>", rendererInfo.flags);
        }
    }

    {
        Uint32 actualFormat = 0;
        int actualAccess = 0;
        int actualWidth = 0;
        int actualHeight = 0;
        if (SDL_QueryTexture(sTexture, &actualFormat, &actualAccess, &actualWidth, &actualHeight) == 0) {
            GX2_PRESENT_LOG("texture requested=0x%08X actual=0x%08X access=%d size=%dx%d",
                            SDL_PIXELFORMAT_RGBA32, actualFormat, actualAccess, actualWidth, actualHeight);
        }
    }

    GX2Present_UpdateDestRect(true);
    return true;
}

void GX2Present_Shutdown(void)
{
    if (sTexture != NULL) {
        SDL_DestroyTexture(sTexture);
        sTexture = NULL;
    }

    if (sRenderer != NULL) {
        SDL_DestroyRenderer(sRenderer);
        sRenderer = NULL;
    }

    SDL_free(sUploadPixels);
    sUploadPixels = NULL;
    sFrameWidth = 0;
    sFrameHeight = 0;
    sLastOutputWidth = 0;
    sLastOutputHeight = 0;
}

bool GX2Present_UploadFrame(const uint16_t *framePixels)
{
    int updateResult;

    if (sTexture == NULL || sUploadPixels == NULL)
        return false;

    if (GX2Present_ShouldTrace())
        GX2_PRESENT_LOG("frame[%u] upload begin", sPresentFrameCounter);

    ConvertFrameToRgba32(sUploadPixels, framePixels);
    updateResult = SDL_UpdateTexture(sTexture, NULL, sUploadPixels, sFrameWidth * (int)sizeof(*sUploadPixels));

    if (updateResult != 0) {
        GX2_PRESENT_LOG("frame[%u] SDL_UpdateTexture failed: %s", sPresentFrameCounter, SDL_GetError());
        return false;
    }

    if (GX2Present_ShouldTrace())
        GX2_PRESENT_LOG("frame[%u] upload end", sPresentFrameCounter);

    return true;
}

void GX2Present_Present(void)
{
    if (sRenderer == NULL || sTexture == NULL)
        return;

    if (GX2Present_ShouldTrace())
        GX2_PRESENT_LOG("frame[%u] present begin", sPresentFrameCounter);

    GX2Present_UpdateDestRect(false);
    SDL_RenderClear(sRenderer);
    SDL_RenderCopy(sRenderer, sTexture, NULL, &sDestRect);
    SDL_RenderPresent(sRenderer);

    if (GX2Present_ShouldTrace())
        GX2_PRESENT_LOG("frame[%u] present end", sPresentFrameCounter);

    sPresentFrameCounter++;
}

#else

bool GX2Present_Init(SDL_Window *window, int frameWidth, int frameHeight)
{
    (void)window;
    (void)frameWidth;
    (void)frameHeight;
    SDL_SetError("GX2 presenter is only available on Wii U");
    return false;
}

void GX2Present_Shutdown(void) { }

bool GX2Present_UploadFrame(const uint16_t *framePixels)
{
    (void)framePixels;
    return false;
}

void GX2Present_Present(void) { }

#endif
