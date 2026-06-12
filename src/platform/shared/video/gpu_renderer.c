#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#if defined(VIDEO_BACKEND_OPENGL)
#include <SDL_opengl.h>
#endif

#include "core.h"
#include "global.h"
#include "background.h"
#include "platform/shared/gpu_renderer.h"
#include "tilemap.h"
#include "trig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define GPU_MAX_SPRITES    256
#define GPU_MAX_TRANSFORMS 256
#define CHUNK_SIZE_PIXELS  96

typedef struct {
    u8 r;
    u8 g;
    u8 b;
    u8 a;
} RgbaPixel;

typedef struct {
    Sprite *sprite;
    SpriteTransform transform;
    bool active;
} StoredTransform;

typedef struct {
    s16 x;
    s16 y;
    u32 frameFlags;
    u8 oamPaletteNum;
    const SpriteOffset *dimensions;
    const void *graphicsSrc;
    bool hasTransform;
    SpriteTransform transform;
} QueuedSprite;

static SDL_Window *sWindow;

#if defined(VIDEO_BACKEND_OPENGL)
static SDL_GLContext sGlContext;
static GLuint sGlTexture;
#endif

static Background sActiveBackgrounds[NUM_BACKGROUNDS];
static bool sHasActiveBackground[NUM_BACKGROUNDS];
static StoredTransform sTransforms[GPU_MAX_TRANSFORMS];
static size_t sTransformCount;
static QueuedSprite sSprites[GPU_MAX_SPRITES];
static size_t sSpriteCount;
static RgbaPixel *sScratchPixels;
static size_t sScratchPixelCapacity;

static u8 Expand5To8(u16 value) { return (u8)((value << 3) | (value >> 2)); }

static RgbaPixel MakeRgbaPixel(u16 color, u8 alpha)
{
    RgbaPixel pixel;
    pixel.r = Expand5To8(color & 0x1F);
    pixel.g = Expand5To8((color >> 5) & 0x1F);
    pixel.b = Expand5To8((color >> 10) & 0x1F);
    pixel.a = alpha;
    return pixel;
}

static bool EnsureScratchPixelCapacity(size_t pixelCount)
{
    if (sScratchPixelCapacity >= pixelCount) {
        return true;
    }

    RgbaPixel *pixels = realloc(sScratchPixels, pixelCount * sizeof(*pixels));
    if (pixels == NULL) {
        SDL_SetError("Failed to allocate %zu RGBA pixels", pixelCount);
        return false;
    }

    sScratchPixels = pixels;
    sScratchPixelCapacity = pixelCount;
    return true;
}

static StoredTransform *FindStoredTransform(Sprite *sprite)
{
    for (size_t i = 0; i < sTransformCount; i++) {
        if (sTransforms[i].active && sTransforms[i].sprite == sprite) {
            return &sTransforms[i];
        }
    }

    return NULL;
}

static u16 ReadPaletteColor(const ColorRaw *palette, u32 index) { return (u16)palette[index]; }

static u16 ReadBgPaletteColor(u8 paletteId, u8 colorId) { return ReadPaletteColor(gBgPalette, paletteId * PALETTE_LEN_4BPP + colorId); }

static u16 ReadObjPaletteColor(u8 paletteId, u8 colorId)
{
    return ReadPaletteColor(gObjPalette, paletteId * PALETTE_LEN_4BPP + colorId);
}

static bool Convert4bppSpriteToRgba(const u8 *bitmap4bpp, int width, int height, u8 paletteId)
{
    if (!EnsureScratchPixelCapacity((size_t)width * height)) {
        return false;
    }

    int widthInTiles = width >> 3;

    for (int frameY = 0; frameY < height; frameY++) {
        for (int frameX = 0; frameX < width; frameX++) {
            int tileIndex = (frameY >> 3) * widthInTiles + (frameX >> 3);
            int tileColorIndex = ((frameY & 0x7) * 8 + (frameX & 0x7)) + (tileIndex * 64);
            int targetColorIndex = (frameY * width) + frameX;
            bool highNibble = (targetColorIndex & 1) != 0;
            u8 pixPair = bitmap4bpp[tileColorIndex >> 1];
            u8 colorId = highNibble ? (pixPair >> 4) : (pixPair & 0xF);
            u16 paletteColor = ReadObjPaletteColor(paletteId, colorId);

            sScratchPixels[targetColorIndex] = MakeRgbaPixel(paletteColor, colorId == 0 ? 0 : 255);
        }
    }

    return true;
}

static bool ConvertTilemapToRgba(const Background *bg, int chunkIndex)
{
    int width = bg->xTiles * 8;
    int height = bg->yTiles * 8;
    bool is4bpp = (gBgCntRegs[bg->flags & BACKGROUND_FLAGS_MASK_BG_ID] & BGCNT_256COLOR) == 0;

    if (!EnsureScratchPixelCapacity((size_t)width * height)) {
        return false;
    }

    for (int tileIdY = 0; tileIdY < bg->yTiles; tileIdY++) {
        for (int tileIdX = 0; tileIdX < bg->xTiles; tileIdX++) {
            RgbaPixel *dstTile = &sScratchPixels[(tileIdY * 8 * width) + tileIdX * 8];

            if (is4bpp) {
                int tileInChunkIndex = (chunkIndex * bg->xTiles * bg->yTiles) + tileIdY * bg->xTiles + tileIdX;
                u16 tileEntry = bg->layout[tileInChunkIndex];
                u16 tileIndex = tileEntry & TileMask_Index;
                bool xFlip = (tileEntry & TileMask_FlipX) != 0;
                bool yFlip = (tileEntry & TileMask_FlipY) != 0;
                u8 paletteId = (tileEntry >> 12) & 0xF;
                const u8 *srcTile = &((const u8 *)bg->graphics.src)[tileIndex * 32];

                for (int y = 0; y < 8; y++) {
                    int srcY = yFlip ? (7 - y) : y;
                    for (int x = 0; x < 8; x++) {
                        int srcX = xFlip ? (7 - x) : x;
                        int pixelIndex = srcY * 8 + srcX;
                        u8 pixPair = srcTile[pixelIndex >> 1];
                        u8 colorId = (pixelIndex & 1) ? (pixPair >> 4) : (pixPair & 0xF);
                        u16 paletteColor = ReadBgPaletteColor(paletteId, colorId);
                        dstTile[(y * width) + x] = MakeRgbaPixel(paletteColor, colorId == 0 ? 0 : 255);
                    }
                }
            } else {
                u8 tileIndex = ((const u8 *)bg->layout)[tileIdY * bg->xTiles + tileIdX];
                const u8 *srcTile = &((const u8 *)bg->graphics.src)[tileIndex * 64];

                for (int y = 0; y < 8; y++) {
                    for (int x = 0; x < 8; x++) {
                        u8 colorId = srcTile[y * 8 + x];
                        u16 paletteColor = ReadPaletteColor(gBgPalette, colorId);
                        dstTile[(y * width) + x] = MakeRgbaPixel(paletteColor, colorId == 0 ? 0 : 255);
                    }
                }
            }
        }
    }

    return true;
}

#if defined(VIDEO_BACKEND_OPENGL)
static void SetupViewport(void)
{
    int drawableWidth;
    int drawableHeight;
    SDL_GL_GetDrawableSize(sWindow, &drawableWidth, &drawableHeight);

    float scaleX = (float)drawableWidth / DISPLAY_WIDTH;
    float scaleY = (float)drawableHeight / DISPLAY_HEIGHT;
    float scale = scaleX < scaleY ? scaleX : scaleY;
    int viewportWidth = (int)(DISPLAY_WIDTH * scale);
    int viewportHeight = (int)(DISPLAY_HEIGHT * scale);
    int viewportX = (drawableWidth - viewportWidth) / 2;
    int viewportY = (drawableHeight - viewportHeight) / 2;

    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void DrawRgbaPixels(const RgbaPixel *pixels, int width, int height, const GLfloat vertices[8], const GLfloat texCoords[8])
{
    glBindTexture(GL_TEXTURE_2D, sGlTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glBegin(GL_TRIANGLE_STRIP);
    glTexCoord2f(texCoords[0], texCoords[1]);
    glVertex2f(vertices[0], vertices[1]);
    glTexCoord2f(texCoords[2], texCoords[3]);
    glVertex2f(vertices[2], vertices[3]);
    glTexCoord2f(texCoords[4], texCoords[5]);
    glVertex2f(vertices[4], vertices[5]);
    glTexCoord2f(texCoords[6], texCoords[7]);
    glVertex2f(vertices[6], vertices[7]);
    glEnd();
}
#endif

static void DrawAxisAlignedRgbaPixels(const RgbaPixel *pixels, int width, int height, float x, float y, bool xFlip, bool yFlip)
{
#if defined(VIDEO_BACKEND_OPENGL)
    GLfloat vertices[8] = { x, y, x + width, y, x, y + height, x + width, y + height };
    GLfloat texCoords[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

    if (xFlip) {
        texCoords[0] = 1.0f;
        texCoords[2] = 0.0f;
        texCoords[4] = 1.0f;
        texCoords[6] = 0.0f;
    }

    if (yFlip) {
        texCoords[1] = 1.0f;
        texCoords[3] = 1.0f;
        texCoords[5] = 0.0f;
        texCoords[7] = 0.0f;
    }

    DrawRgbaPixels(pixels, width, height, vertices, texCoords);
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)x;
    (void)y;
    (void)xFlip;
    (void)yFlip;
#endif
}

static void DrawLevelMapBackground(const Background *bg)
{
    int mapChunkX = bg->scrollX / CHUNK_SIZE_PIXELS;
    int mapChunkY = bg->scrollY / CHUNK_SIZE_PIXELS;
    int screenX = -(bg->scrollX % CHUNK_SIZE_PIXELS);
    int screenY = -(bg->scrollY % CHUNK_SIZE_PIXELS);
    int visibleChunkWidth = ((DISPLAY_WIDTH + CHUNK_SIZE_PIXELS - 1) / CHUNK_SIZE_PIXELS) + 1;
    int visibleChunkHeight = ((DISPLAY_HEIGHT + CHUNK_SIZE_PIXELS - 1) / CHUNK_SIZE_PIXELS) + 1;

    if (visibleChunkWidth > (bg->mapWidth - mapChunkX)) {
        visibleChunkWidth = bg->mapWidth - mapChunkX;
    }
    if (visibleChunkHeight > (bg->mapHeight - mapChunkY)) {
        visibleChunkHeight = bg->mapHeight - mapChunkY;
    }

    for (int chunkY = 0; chunkY < visibleChunkHeight; chunkY++) {
        for (int chunkX = 0; chunkX < visibleChunkWidth; chunkX++) {
            int chunkPosIndex = (mapChunkY + chunkY) * bg->mapWidth + (mapChunkX + chunkX);
            int chunkId = bg->metatileMap[chunkPosIndex] & TILE_MASK_INDEX;

            if (!ConvertTilemapToRgba(bg, chunkId)) {
                return;
            }

            DrawAxisAlignedRgbaPixels(sScratchPixels, CHUNK_SIZE_PIXELS, CHUNK_SIZE_PIXELS, (float)(screenX + chunkX * CHUNK_SIZE_PIXELS),
                                      (float)(screenY + chunkY * CHUNK_SIZE_PIXELS), false, false);
        }
    }
}

static void DrawRegularBackground(const Background *bg, u8 bgId)
{
    float x = (float)gBgScrollRegs[bgId][0];
    float y = (float)gBgScrollRegs[bgId][1];

    if (!ConvertTilemapToRgba(bg, 0)) {
        return;
    }

    DrawAxisAlignedRgbaPixels(sScratchPixels, bg->xTiles * 8, bg->yTiles * 8, x, y, false, false);
}

static void RenderBackground(const Background *bg)
{
    u8 bgId = bg->flags & BACKGROUND_FLAGS_MASK_BG_ID;
    if ((gDispCnt & (DISPCNT_BG0_ON << bgId)) == 0) {
        return;
    }

    if (bg->flags & BACKGROUND_FLAG_IS_LEVEL_MAP) {
        DrawLevelMapBackground(bg);
    } else {
        DrawRegularBackground(bg, bgId);
    }
}

static void RenderQueuedSprite(const QueuedSprite *sprite)
{
    const SpriteOffset *dims = sprite->dimensions;
    if (dims == NULL || dims == (void *)-1 || sprite->graphicsSrc == NULL) {
        return;
    }

    if (!Convert4bppSpriteToRgba(sprite->graphicsSrc, dims->width, dims->height, sprite->oamPaletteNum)) {
        return;
    }

    bool xFlip = (sprite->frameFlags & SPRITE_FLAG_MASK_X_FLIP) != 0;
    bool yFlip = (sprite->frameFlags & SPRITE_FLAG_MASK_Y_FLIP) != 0;

    if (!sprite->hasTransform) {
        int x = sprite->x;
        int y = sprite->y;

        if (sprite->frameFlags & SPRITE_FLAG_GLOBAL_OFFSET) {
            x -= gSpriteOffset.x;
            y -= gSpriteOffset.y;
        }

        if (yFlip) {
            y -= dims->height - dims->offsetY;
        } else {
            y -= dims->offsetY;
        }

        if (xFlip) {
            x -= dims->width - dims->offsetX;
        } else {
            x -= dims->offsetX;
        }

        DrawAxisAlignedRgbaPixels(sScratchPixels, dims->width, dims->height, (float)x, (float)y, xFlip, yFlip);
        return;
    }

#if defined(VIDEO_BACKEND_OPENGL)
    GLfloat vertices[8];
    GLfloat texCoords[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };
    float rotation = -((float)(sprite->transform.rotation & ONE_CYCLE) / 1024.0f) * (2.0f * (float)M_PI);
    float sinTheta = sinf(rotation);
    float cosTheta = cosf(rotation);
    float scaleX = (float)sprite->transform.qScaleX / 256.0f;
    float scaleY = (float)sprite->transform.qScaleY / 256.0f;
    float anchorX = (float)sprite->transform.x;
    float anchorY = (float)sprite->transform.y;
    float pivotX = scaleX >= 0.0f ? dims->offsetX : (dims->width - dims->offsetX);
    float pivotY = scaleY >= 0.0f ? dims->offsetY : (dims->height - dims->offsetY);
    float corners[4][2] = {
        { 0.0f, 0.0f },
        { (float)dims->width, 0.0f },
        { 0.0f, (float)dims->height },
        { (float)dims->width, (float)dims->height },
    };

    if (sprite->frameFlags & SPRITE_FLAG_GLOBAL_OFFSET) {
        anchorX -= gSpriteOffset.x;
        anchorY -= gSpriteOffset.y;
    }

    if (xFlip) {
        texCoords[0] = 1.0f;
        texCoords[2] = 0.0f;
        texCoords[4] = 1.0f;
        texCoords[6] = 0.0f;
    }

    if (yFlip) {
        texCoords[1] = 1.0f;
        texCoords[3] = 1.0f;
        texCoords[5] = 0.0f;
        texCoords[7] = 0.0f;
    }

    for (int i = 0; i < 4; i++) {
        float localX = (corners[i][0] - pivotX) * scaleX;
        float localY = (corners[i][1] - pivotY) * scaleY;
        vertices[i * 2 + 0] = anchorX + (localX * cosTheta) - (localY * sinTheta);
        vertices[i * 2 + 1] = anchorY + (localX * sinTheta) + (localY * cosTheta);
    }

    DrawRgbaPixels(sScratchPixels, dims->width, dims->height, vertices, texCoords);
#endif
}

static void RenderScene(void)
{
#if defined(VIDEO_BACKEND_OPENGL)
    u16 backdropColor = ReadPaletteColor(gBgPalette, 0);
    RgbaPixel backdrop = MakeRgbaPixel(backdropColor, 255);

    SetupViewport();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor((float)backdrop.r / 255.0f, (float)backdrop.g / 255.0f, (float)backdrop.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    for (int priority = 3; priority >= 0; priority--) {
        for (int bgId = NUM_BACKGROUNDS - 1; bgId >= 0; bgId--) {
            if (sHasActiveBackground[bgId] && ((gBgCntRegs[bgId] & 0x3) == priority)) {
                RenderBackground(&sActiveBackgrounds[bgId]);
            }
        }

        for (size_t i = 0; i < sSpriteCount; i++) {
            if (SPRITE_FLAG_GET((&sSprites[i]), PRIORITY) == (u32)priority) {
                RenderQueuedSprite(&sSprites[i]);
            }
        }
    }

    SDL_GL_SwapWindow(sWindow);
#endif
}

bool GpuRenderer_Init(SDL_Window *window)
{
    sWindow = window;
    memset(sHasActiveBackground, 0, sizeof(sHasActiveBackground));
    sTransformCount = 0;
    sSpriteCount = 0;

#if defined(VIDEO_BACKEND_OPENGL)
    sGlContext = SDL_GL_CreateContext(window);
    if (sGlContext == NULL) {
        return false;
    }

    if (SDL_GL_SetSwapInterval(1) != 0) {
        SDL_Log("OpenGL swap interval setup failed: %s", SDL_GetError());
    }

    glGenTextures(1, &sGlTexture);
    return sGlTexture != 0;
#elif defined(VIDEO_BACKEND_VULKAN)
    SDL_SetError("Vulkan backend is not implemented yet");
    return false;
#else
    SDL_SetError("No GPU backend selected");
    return false;
#endif
}

void GpuRenderer_Shutdown(void)
{
#if defined(VIDEO_BACKEND_OPENGL)
    if (sGlTexture != 0) {
        glDeleteTextures(1, &sGlTexture);
        sGlTexture = 0;
    }

    if (sGlContext != NULL) {
        SDL_GL_DeleteContext(sGlContext);
        sGlContext = NULL;
    }
#endif

    free(sScratchPixels);
    sScratchPixels = NULL;
    sScratchPixelCapacity = 0;
    sWindow = NULL;
}

void GpuRenderer_ProcessBackgroundsCopyQueue(void)
{
    while (gBackgroundsCopyQueueCursor != gBackgroundsCopyQueueIndex) {
        Background *bg = gBackgroundsCopyQueue[gBackgroundsCopyQueueCursor];
        INC_BACKGROUNDS_QUEUE_CURSOR(gBackgroundsCopyQueueCursor);

        if ((bg->flags & BACKGROUND_FLAG_20) && (bg->scrollX == bg->prevScrollX) && (bg->scrollY == bg->prevScrollY)) {
            continue;
        }

        u8 bgId = bg->flags & BACKGROUND_FLAGS_MASK_BG_ID;
        sActiveBackgrounds[bgId] = *bg;
        sHasActiveBackground[bgId] = true;
    }

    RenderScene();
    sSpriteCount = 0;
    sTransformCount = 0;
}

void GpuRenderer_TransformSprite(Sprite *sprite, SpriteTransform *transform)
{
    StoredTransform *stored = FindStoredTransform(sprite);

    if (stored == NULL) {
        if (sTransformCount >= GPU_MAX_TRANSFORMS) {
            return;
        }

        stored = &sTransforms[sTransformCount++];
        stored->sprite = sprite;
        stored->active = true;
    }

    stored->transform = *transform;
}

void GpuRenderer_DisplaySprite(Sprite *sprite, u8 oamPaletteNum)
{
    if (sprite == NULL || sprite->graphics.src == NULL || sprite->dimensions == (void *)-1) {
        return;
    }

    if (sSpriteCount >= GPU_MAX_SPRITES) {
        return;
    }

    QueuedSprite *queued = &sSprites[sSpriteCount++];
    StoredTransform *stored = FindStoredTransform(sprite);

    queued->x = sprite->x;
    queued->y = sprite->y;
    queued->frameFlags = sprite->frameFlags;
    queued->oamPaletteNum = oamPaletteNum;
    queued->dimensions = sprite->dimensions;
    queued->graphicsSrc = sprite->graphics.src;
    queued->hasTransform = stored != NULL;

    if (stored != NULL) {
        queued->transform = stored->transform;
    }
}
