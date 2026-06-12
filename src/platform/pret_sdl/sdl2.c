#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __PSP__
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspgu.h>
#endif

#ifdef PLATFORM_WIIU
#include <coreinit/debug.h>
#endif

#include <SDL.h>

// NOTE(wiiu): SDL2-wiiu calls ProcUIInitEx internally during SDL_Init.
// Do NOT call WHBProcInit()/WHBProcShutdown().

#include "global.h"
#include "core.h"
#include "lib/agb_flash/flash_internal.h"
#include "platform/shared/dma.h"
#include "platform/shared/input.h"
#if defined(USE_PLATFORM_RENDERER)
#include "platform/shared/gpu_renderer.h"
#endif
#if defined(PLATFORM_WIIU) && !defined(USE_PLATFORM_RENDERER)
#include "platform/shared/video/GX2Present.h"
#endif
#include "platform/shared/video/gpsp_renderer.h"

#if ENABLE_AUDIO
#include "platform/shared/audio/cgb_audio.h"
#endif

ALIGNED(256) uint16_t gameImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
ALIGNED(256) static uint16_t textureUploadImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
#endif

#if ENABLE_VRAM_VIEW
uint16_t vramBuffer[VRAM_VIEW_WIDTH * VRAM_VIEW_HEIGHT];
#endif

SDL_Window *sdlWindow;
#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
#endif
#if ENABLE_VRAM_VIEW
SDL_Window *vramWindow;
SDL_Renderer *vramRenderer;
SDL_Texture *vramTexture;
#endif
#define INITIAL_VIDEO_SCALE 1
unsigned int videoScale = INITIAL_VIDEO_SCALE;
unsigned int preFullscreenVideoScale = INITIAL_VIDEO_SCALE;

bool speedUp = false;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
bool stepOneFrame = false;
bool headless = false;

#if ENABLE_AUDIO
static SDL_AudioDeviceID sdlAudioDevice = 0;
#endif

#ifdef __PSP__
static SDL_Joystick *joystick = NULL;
#endif

#ifdef PLATFORM_WIIU
#define WIIU_LOG(fmt, ...) OSReport("[sa2-wiiu] " fmt "\n", ##__VA_ARGS__)
#define WIIU_MAX_CONTROLLERS 5  // GamePad + 4 Pro Controllers.
#define WIIU_AXIS_DEADZONE 12000

typedef struct {
    SDL_GameController *controller;
    SDL_Joystick *joystick;
    SDL_JoystickID instanceId;
    bool isGameController;
} WiiUControllerSlot;

static WiiUControllerSlot sWiiUControllers[WIIU_MAX_CONTROLLERS];
static int sWiiUNumControllers = 0;
static u16 sWiiULastControllerKeys = 0;

static int WiiUFindControllerSlot(SDL_JoystickID instanceId)
{
    for (int i = 0; i < sWiiUNumControllers; i++) {
        if (sWiiUControllers[i].instanceId == instanceId) {
            return i;
        }
    }

    return -1;
}

static void WiiUCloseControllerSlot(int slot)
{
    WiiUControllerSlot *controller = &sWiiUControllers[slot];
    SDL_JoystickID instanceId = controller->instanceId;

    if (controller->isGameController && controller->controller != NULL) {
        SDL_GameControllerClose(controller->controller);
    } else if (controller->joystick != NULL) {
        SDL_JoystickClose(controller->joystick);
    }

    sWiiUControllers[slot] = sWiiUControllers[--sWiiUNumControllers];
    memset(&sWiiUControllers[sWiiUNumControllers], 0, sizeof(sWiiUControllers[sWiiUNumControllers]));
    WIIU_LOG("closed controller instance %d, %d remaining", instanceId, sWiiUNumControllers);
}

static void WiiUCloseAllControllers(void)
{
    while (sWiiUNumControllers > 0) {
        WiiUCloseControllerSlot(sWiiUNumControllers - 1);
    }
}

static void WiiUOpenController(int deviceIndex)
{
    if (sWiiUNumControllers >= WIIU_MAX_CONTROLLERS) {
        WIIU_LOG("  joystick %d skipped, controller table full", deviceIndex);
        return;
    }

    if (SDL_IsGameController(deviceIndex)) {
        SDL_GameController *controller = SDL_GameControllerOpen(deviceIndex);
        SDL_Joystick *joystick;
        SDL_JoystickID instanceId;

        if (controller == NULL) {
            WIIU_LOG("  controller %d open failed: %s", deviceIndex, SDL_GetError());
            return;
        }

        joystick = SDL_GameControllerGetJoystick(controller);
        if (joystick == NULL) {
            WIIU_LOG("  controller %d has no joystick handle: %s", deviceIndex, SDL_GetError());
            SDL_GameControllerClose(controller);
            return;
        }

        instanceId = SDL_JoystickInstanceID(joystick);

        if (WiiUFindControllerSlot(instanceId) >= 0) {
            SDL_GameControllerClose(controller);
            return;
        }

        WiiUControllerSlot *slot = &sWiiUControllers[sWiiUNumControllers++];
        slot->controller = controller;
        slot->joystick = joystick;
        slot->instanceId = instanceId;
        slot->isGameController = true;
        WIIU_LOG("  opened controller %d instance=%d name=%s axes=%d buttons=%d",
                 deviceIndex, instanceId,
                 (SDL_GameControllerName(controller) != NULL) ? SDL_GameControllerName(controller) : "<null>",
                 SDL_JoystickNumAxes(joystick), SDL_JoystickNumButtons(joystick));
        return;
    }

    SDL_Joystick *joystick = SDL_JoystickOpen(deviceIndex);
    if (joystick == NULL) {
        WIIU_LOG("  joystick %d raw open failed: %s", deviceIndex, SDL_GetError());
        return;
    }

    SDL_JoystickID instanceId = SDL_JoystickInstanceID(joystick);
    if (WiiUFindControllerSlot(instanceId) >= 0) {
        SDL_JoystickClose(joystick);
        return;
    }

    WiiUControllerSlot *slot = &sWiiUControllers[sWiiUNumControllers++];
    slot->controller = NULL;
    slot->joystick = joystick;
    slot->instanceId = instanceId;
    slot->isGameController = false;
    WIIU_LOG("  opened raw joystick %d instance=%d name=%s axes=%d buttons=%d hats=%d",
             deviceIndex, instanceId,
             (SDL_JoystickName(joystick) != NULL) ? SDL_JoystickName(joystick) : "<null>",
             SDL_JoystickNumAxes(joystick), SDL_JoystickNumButtons(joystick), SDL_JoystickNumHats(joystick));
}

static void WiiUOpenAllControllers(void)
{
    int numJoysticks = SDL_NumJoysticks();
    WiiUCloseAllControllers();
    WIIU_LOG("WiiUOpenAllControllers: %d joystick(s) detected", numJoysticks);
    for (int i = 0; i < numJoysticks && sWiiUNumControllers < WIIU_MAX_CONTROLLERS; i++) {
        WiiUOpenController(i);
    }
    WIIU_LOG("WiiUOpenAllControllers: %d controller(s) open", sWiiUNumControllers);
}

static void WiiUCloseController(SDL_JoystickID instanceId)
{
    int slot = WiiUFindControllerSlot(instanceId);

    if (slot >= 0) {
        WiiUCloseControllerSlot(slot);
    }
}
#endif

#ifdef __PSP__
static SDL_Rect pspDestRect;
#endif

double timeScale = 1.0;
enum { PORTABLE_AUDIO_SAMPLES_PER_FRAME = 800 };

static FILE *sSaveFile = NULL;

#ifdef PLATFORM_WIIU
enum {
    WIIU_TRACE_WINDOW = 90,
    WIIU_TRACE_INTERVAL = 300,
};

static u32 sWiiURenderFrameCounter = 0;
static u32 sWiiUAudioQueueCounter = 0;
static bool sWiiUVBlankLogged = false;

static bool WiiUShouldTraceCounter(u32 counter)
{
    return (counter < WIIU_TRACE_WINDOW) || ((counter % WIIU_TRACE_INTERVAL) == 0);
}

#endif

#if ENABLE_AUDIO && defined(PLATFORM_WIIU)
enum {
    WIIU_AUDIO_SAMPLE_FRAME_BYTES = sizeof(s16) * 2,
    WIIU_AUDIO_PUSH_FRAME_BYTES = PORTABLE_AUDIO_SAMPLES_PER_FRAME * WIIU_AUDIO_SAMPLE_FRAME_BYTES,
    WIIU_AUDIO_TARGET_BUFFERED_FRAMES = 20,
    WIIU_AUDIO_BUFFERED_FRAMES = 40,
    WIIU_AUDIO_TARGET_BUFFER_SIZE = WIIU_AUDIO_PUSH_FRAME_BYTES * WIIU_AUDIO_TARGET_BUFFERED_FRAMES,
    WIIU_AUDIO_BUFFER_SIZE = WIIU_AUDIO_PUSH_FRAME_BYTES * WIIU_AUDIO_BUFFERED_FRAMES,
};

static Uint8 sWiiUAudioBuffer[WIIU_AUDIO_BUFFER_SIZE];
static size_t sWiiUAudioReadPos = 0;
static size_t sWiiUAudioWritePos = 0;
static size_t sWiiUAudioBufferedBytes = 0;
static bool sWiiUAudioEnabled = false;
static u32 sWiiUAudioCallbackCounter = 0;
static u32 sWiiUAudioUnderflowCounter = 0;
static u32 sWiiUAudioDroppedBytes = 0;

typedef struct {
    u32 sampleCount;
    u32 nonzeroCount;
    u32 peak;
    s16 minSample;
    s16 maxSample;
    s16 firstSamples[4];
} WiiUAudioStats;

static void WiiUAudioReset(void)
{
    sWiiUAudioReadPos = 0;
    sWiiUAudioWritePos = 0;
    sWiiUAudioBufferedBytes = 0;
    sWiiUAudioCallbackCounter = 0;
    sWiiUAudioUnderflowCounter = 0;
    sWiiUAudioDroppedBytes = 0;
}

static WiiUAudioStats WiiUAudioGatherStats(const s16 *data, u32 bytesCount)
{
    WiiUAudioStats stats;
    memset(&stats, 0, sizeof(stats));

    stats.sampleCount = bytesCount / sizeof(s16);
    if (data == NULL || stats.sampleCount == 0) {
        return stats;
    }

    stats.minSample = data[0];
    stats.maxSample = data[0];

    for (u32 i = 0; i < stats.sampleCount; i++) {
        s16 sample = data[i];
        s32 absSample;

        if (i < ARRAY_COUNT(stats.firstSamples)) {
            stats.firstSamples[i] = sample;
        }

        if (sample != 0) {
            stats.nonzeroCount++;
        }

        if (sample < stats.minSample) {
            stats.minSample = sample;
        }

        if (sample > stats.maxSample) {
            stats.maxSample = sample;
        }

        absSample = (sample < 0) ? -(s32)sample : sample;
        if ((u32)absSample > stats.peak) {
            stats.peak = (u32)absSample;
        }
    }

    return stats;
}

static void SDLCALL WiiUAudioCallback(void *userdata, Uint8 *stream, int len)
{
    size_t toCopy;
    size_t firstChunk;

    (void)userdata;

    sWiiUAudioCallbackCounter++;

    if (len <= 0) {
        return;
    }

    SDL_memset(stream, 0, (size_t)len);

    if (sWiiUAudioBufferedBytes == 0) {
        sWiiUAudioUnderflowCounter++;
        return;
    }

    toCopy = SDL_min((size_t)len, sWiiUAudioBufferedBytes);
    firstChunk = SDL_min(toCopy, WIIU_AUDIO_BUFFER_SIZE - sWiiUAudioReadPos);

    SDL_memcpy(stream, &sWiiUAudioBuffer[sWiiUAudioReadPos], firstChunk);
    if (toCopy > firstChunk) {
        SDL_memcpy(stream + firstChunk, sWiiUAudioBuffer, toCopy - firstChunk);
    }

    if (toCopy < (size_t)len) {
        sWiiUAudioUnderflowCounter++;
    }

    sWiiUAudioReadPos = (sWiiUAudioReadPos + toCopy) % WIIU_AUDIO_BUFFER_SIZE;
    sWiiUAudioBufferedBytes -= toCopy;
}

static void WiiUAudioPush(const Uint8 *data, size_t len)
{
    size_t firstChunk;

    if (len == 0) {
        return;
    }

    if (len > WIIU_AUDIO_BUFFER_SIZE) {
        data += len - WIIU_AUDIO_BUFFER_SIZE;
        len = WIIU_AUDIO_BUFFER_SIZE;
    }

    if (sWiiUAudioBufferedBytes + len > WIIU_AUDIO_TARGET_BUFFER_SIZE) {
        size_t drop = sWiiUAudioBufferedBytes + len - WIIU_AUDIO_TARGET_BUFFER_SIZE;
        drop = (drop + WIIU_AUDIO_SAMPLE_FRAME_BYTES - 1) & ~(WIIU_AUDIO_SAMPLE_FRAME_BYTES - 1);
        if (drop > sWiiUAudioBufferedBytes) {
            drop = sWiiUAudioBufferedBytes;
        }
        sWiiUAudioReadPos = (sWiiUAudioReadPos + drop) % WIIU_AUDIO_BUFFER_SIZE;
        sWiiUAudioBufferedBytes -= drop;
        sWiiUAudioDroppedBytes += (u32)drop;
    }

    if (len > (WIIU_AUDIO_BUFFER_SIZE - sWiiUAudioBufferedBytes)) {
        size_t drop = len - (WIIU_AUDIO_BUFFER_SIZE - sWiiUAudioBufferedBytes);
        sWiiUAudioReadPos = (sWiiUAudioReadPos + drop) % WIIU_AUDIO_BUFFER_SIZE;
        sWiiUAudioBufferedBytes -= drop;
        sWiiUAudioDroppedBytes += (u32)drop;
    }

    firstChunk = SDL_min(len, WIIU_AUDIO_BUFFER_SIZE - sWiiUAudioWritePos);

    SDL_memcpy(&sWiiUAudioBuffer[sWiiUAudioWritePos], data, firstChunk);
    if (len > firstChunk) {
        SDL_memcpy(sWiiUAudioBuffer, data + firstChunk, len - firstChunk);
    }

    sWiiUAudioWritePos = (sWiiUAudioWritePos + len) % WIIU_AUDIO_BUFFER_SIZE;
    sWiiUAudioBufferedBytes += len;
}
#endif

extern void AgbMain(void);
void DoSoftReset(void) {};

void ProcessSDLEvents(void);
#if !defined(USE_PLATFORM_RENDERER)
void VDraw(void);
#endif
void VramDraw(SDL_Texture *texture);

static void ReadSaveFile(char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);
#if !defined(USE_PLATFORM_RENDERER)
#if !defined(PLATFORM_WIIU)
static SDL_Renderer *CreateMainRenderer(SDL_Window *window);
#endif
static void UploadGameTexture(void);
#endif

u16 Platform_GetKeyInput(void);

#ifdef _WIN32
void *Platform_malloc(size_t numBytes) { return HeapAlloc(GetProcessHeap(), HEAP_GENERATE_EXCEPTIONS | HEAP_ZERO_MEMORY, numBytes); }
void Platform_free(void *ptr) { HeapFree(GetProcessHeap(), 0, ptr); }
#endif

#ifdef __PSP__
PSP_MODULE_INFO("SonicAdvance2", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(-1024);

unsigned int sce_newlib_stack_size = 512 * 1024;

extern bool isRunning;

int exitCallback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    isRunning = false;
    return 0;
}

int callbackThread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setupPspCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callbackThread, 0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}
#endif

#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
static SDL_Renderer *CreateMainRenderer(SDL_Window *window)
{
#ifdef __PSP__
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL)
        renderer = SDL_CreateRenderer(window, -1, 0);
    return renderer;
#elif defined(PLATFORM_WIIU)
    static const Uint32 rendererAttempts[] = {
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC,
        SDL_RENDERER_PRESENTVSYNC,
        SDL_RENDERER_ACCELERATED,
        0,
    };

    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1");

    for (size_t attemptIndex = 0; attemptIndex < ARRAY_COUNT(rendererAttempts); attemptIndex++) {
        SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, rendererAttempts[attemptIndex]);
        if (renderer != NULL) {
            return renderer;
        }
    }

    return NULL;
#else
    static const struct {
        const char *name;
        Uint32 flags;
    } rendererAttempts[] = {
        { "opengl", SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC },
        { "opengl", SDL_RENDERER_ACCELERATED },
        { NULL, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC },
        { NULL, SDL_RENDERER_PRESENTVSYNC },
        { NULL, 0 },
    };

    for (size_t attemptIndex = 0; attemptIndex < ARRAY_COUNT(rendererAttempts); attemptIndex++) {
        int rendererIndex = -1;

        if (rendererAttempts[attemptIndex].name != NULL) {
            int numRenderDrivers = SDL_GetNumRenderDrivers();

            for (int driverIndex = 0; driverIndex < numRenderDrivers; driverIndex++) {
                SDL_RendererInfo info;

                if (SDL_GetRenderDriverInfo(driverIndex, &info) != 0) {
                    continue;
                }

                if (info.name != NULL && strcmp(info.name, rendererAttempts[attemptIndex].name) == 0) {
                    rendererIndex = driverIndex;
                    break;
                }
            }

            if (rendererIndex < 0) {
                continue;
            }
        }

        SDL_Renderer *renderer = SDL_CreateRenderer(window, rendererIndex, rendererAttempts[attemptIndex].flags);
        if (renderer != NULL) {
            SDL_RendererInfo info;

            if (SDL_GetRendererInfo(renderer, &info) == 0 && info.name != NULL) {
                SDL_Log("Using SDL renderer backend: %s", info.name);
            }

            return renderer;
        }
    }

    return NULL;
#endif
}
#endif

#if ENABLE_AUDIO
static void PauseAudioPlayback(int pauseOn)
{
    if (sdlAudioDevice != 0) {
        SDL_PauseAudioDevice(sdlAudioDevice, pauseOn);
    }
}

static void ClearQueuedAudioPlayback(void)
{
    if (sdlAudioDevice != 0) {
        SDL_ClearQueuedAudio(sdlAudioDevice);
    }
}

static Uint32 GetQueuedAudioPlaybackSize(void)
{
    if (sdlAudioDevice == 0) {
        return 0;
    }

    return SDL_GetQueuedAudioSize(sdlAudioDevice);
}

static void LockAudioPlayback(void)
{
    if (sdlAudioDevice != 0) {
        SDL_LockAudioDevice(sdlAudioDevice);
    }
}

static void UnlockAudioPlayback(void)
{
    if (sdlAudioDevice != 0) {
        SDL_UnlockAudioDevice(sdlAudioDevice);
    }
}
#endif

#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
static u16 MakeOpaqueABGR1555(u16 pixel)
{
    return (u16)(pixel | 0x8000);
}

static void CopyFrameToTextureBuffer(u16 *dst, const u16 *src)
{
    for (size_t i = 0; i < ARRAY_COUNT(textureUploadImage); i++) {
        dst[i] = MakeOpaqueABGR1555(src[i]);
    }
}
#endif

int main(int argc, char **argv)
{
#ifdef __PSP__
    setupPspCallbacks();
#endif

#ifdef PLATFORM_WIIU
    WIIU_LOG("main start argc=%d", argc);
#endif

    const char *headlessEnv = getenv("HEADLESS");

    if (headlessEnv && strcmp(headlessEnv, "true") == 0) {
        headless = true;
    }

    const char *parentEnv = getenv("SIO_PARENT");

    if (parentEnv && strcmp(parentEnv, "true") == 0) {
        SIO_MULTI_CNT->id = 0;
        SIO_MULTI_CNT->si = 1;
        SIO_MULTI_CNT->sd = 1;
        SIO_MULTI_CNT->enable = false;
    }

    // Open an output console on Windows
#if (defined _WIN32) && (DEBUG != 0)
    AllocConsole();
    AttachConsole(GetCurrentProcessId());
    freopen("CON", "w", stdout);
#endif

    ReadSaveFile("sa2.sav");

#ifdef PLATFORM_WIIU
    WIIU_LOG("save file loaded");
#endif

    // Prevent the multiplayer screen from being drawn ( see core.c:EngineInit() )
    REG_RCNT = 0x8000;
    REG_KEYINPUT = 0x3FF;

    if (headless) {
#if ENABLE_AUDIO
        // Required or it makes an infinite loop
        cgb_audio_init(48000);
#endif
        AgbMain();
        return 1;
    }

    Uint32 sdlInitFlags = SDL_INIT_VIDEO | SDL_INIT_JOYSTICK;
#ifdef PLATFORM_WIIU
    sdlInitFlags |= SDL_INIT_GAMECONTROLLER;
#endif

#if ENABLE_AUDIO
    sdlInitFlags |= SDL_INIT_AUDIO;
#endif

    if (SDL_Init(sdlInitFlags) < 0) {
#ifdef PLATFORM_WIIU
        WIIU_LOG("SDL_Init failed: %s", SDL_GetError());
#endif
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#ifdef PLATFORM_WIIU
    WIIU_LOG("SDL_Init ok flags=0x%08X", sdlInitFlags);
#endif

#ifdef __PSP__
    if (SDL_NumJoysticks() > 0) {
        joystick = SDL_JoystickOpen(0);
    }
#endif
#ifdef PLATFORM_WIIU
    WiiUOpenAllControllers();
#endif

#ifdef TITLE_BAR
    const char *title = STR(TITLE_BAR);
#else
    const char *title = "SAT-R sa2";
#endif

#ifdef __PSP__
    sdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 480, 272, SDL_WINDOW_SHOWN);
#else
    Uint32 mainWindowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#if defined(PLATFORM_WIIU)
    // Wii U: go fullscreen and let SDL scale the same framebuffer size used by PC SDL.
    mainWindowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP;
#elif defined(VIDEO_BACKEND_OPENGL)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    mainWindowFlags |= SDL_WINDOW_OPENGL;
#elif defined(VIDEO_BACKEND_VULKAN)
    mainWindowFlags |= SDL_WINDOW_VULKAN;
#endif
    sdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale,
                                 DISPLAY_HEIGHT * videoScale, mainWindowFlags);
#endif
    if (sdlWindow == NULL) {
#ifdef PLATFORM_WIIU
        WIIU_LOG("SDL_CreateWindow failed: %s", SDL_GetError());
#endif
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#ifdef PLATFORM_WIIU
    WIIU_LOG("window created flags=0x%08X size=%ux%u", mainWindowFlags, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
#endif

#if ENABLE_VRAM_VIEW
    int mainWindowX;
    int mainWindowWidth;
    SDL_GetWindowPosition(sdlWindow, &mainWindowX, NULL);
    SDL_GetWindowSize(sdlWindow, &mainWindowWidth, NULL);
    int vramWindowX = mainWindowX + mainWindowWidth;
    u16 vramWindowWidth = VRAM_VIEW_WIDTH;
    u16 vramWindowHeight = VRAM_VIEW_HEIGHT;
    vramWindow = SDL_CreateWindow("VRAM View", vramWindowX, SDL_WINDOWPOS_CENTERED, vramWindowWidth, vramWindowHeight,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (vramWindow == NULL) {
        fprintf(stderr, "VRAM Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#endif

#if defined(USE_PLATFORM_RENDERER)
    if (!GpuRenderer_Init(sdlWindow)) {
        fprintf(stderr, "GPU renderer could not be initialized! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#elif defined(PLATFORM_WIIU)
    if (!GX2Present_Init(sdlWindow, DISPLAY_WIDTH, DISPLAY_HEIGHT)) {
        WIIU_LOG("GX2 presenter init failed: %s", SDL_GetError());
        fprintf(stderr, "GX2 presenter could not be initialized! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#else
    sdlRenderer = CreateMainRenderer(sdlWindow);
    if (sdlRenderer == NULL) {
        fprintf(stderr, "Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#endif

#if ENABLE_VRAM_VIEW
    vramRenderer = SDL_CreateRenderer(vramWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (vramRenderer == NULL) {
        fprintf(stderr, "VRAM Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
#endif

#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
#ifdef __PSP__
    // SDL_RenderSetLogicalSize is broken on PSP, stretch to fill manually
    pspDestRect = (SDL_Rect) { 0, 0, GU_SCR_WIDTH, GU_SCR_HEIGHT };
#else
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#endif
#endif
#if ENABLE_VRAM_VIEW
    SDL_SetRenderDrawColor(vramRenderer, 0, 0, 0, 255);
    SDL_RenderClear(vramRenderer);
    SDL_RenderSetLogicalSize(vramRenderer, vramWindowWidth, vramWindowHeight);
#endif

#if !defined(USE_PLATFORM_RENDERER) && !defined(PLATFORM_WIIU)
    sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL) {
        fprintf(stderr, "Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureBlendMode(sdlTexture, SDL_BLENDMODE_NONE);
#endif

#if ENABLE_VRAM_VIEW
    vramTexture = SDL_CreateTexture(vramRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, vramWindowWidth, vramWindowHeight);
    if (vramTexture == NULL) {
        fprintf(stderr, "Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureBlendMode(vramTexture, SDL_BLENDMODE_NONE);
#endif

#if ENABLE_AUDIO
    SDL_AudioSpec want;
    SDL_AudioSpec obtained;
#if defined(PLATFORM_WIIU)
    // Wii U hardware natively runs at 48000 Hz, matching the PC SDL mix rate.
    const int audioFrequency = 48000;
#else
    const int audioFrequency = PORTABLE_AUDIO_SAMPLES_PER_FRAME * 60;
#endif

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = audioFrequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = PORTABLE_AUDIO_SAMPLES_PER_FRAME;
#if defined(PLATFORM_WIIU)
    want.callback = WiiUAudioCallback;
    WiiUAudioReset();
    sWiiUAudioEnabled = false;
#endif
    cgb_audio_init(audioFrequency);

    SDL_memset(&obtained, 0, sizeof(obtained));
    sdlAudioDevice = 0;

#if defined(PLATFORM_WIIU)
    // Use callback mode on Wii U. Queue mode was crashing asynchronously.
    WIIU_LOG("audio open request freq=%d format=0x%X channels=%u samples=%u callback=1", want.freq, want.format, want.channels, want.samples);
    sdlAudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &obtained, 0);
    if (sdlAudioDevice == 0) {
        WIIU_LOG("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    } else {
        WIIU_LOG("audio open ok device=%u freq=%d format=0x%X channels=%u samples=%u", sdlAudioDevice, obtained.freq,
                 obtained.format, obtained.channels, obtained.samples);
        sWiiUAudioEnabled = true;
        PauseAudioPlayback(0);
        WIIU_LOG("audio playback resumed");
    }
#else
    sdlAudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &obtained, 0);
    if (sdlAudioDevice == 0) {
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    } else {
        if (obtained.format != AUDIO_S16SYS) /* we let this one thing change. */
            SDL_Log("We didn't get S16 audio format.");
        PauseAudioPlayback(0);
    }
#endif
#endif

#if !defined(USE_PLATFORM_RENDERER)
    VDraw();
#endif
#if ENABLE_VRAM_VIEW
    VramDraw(vramTexture);
#endif

#ifdef PLATFORM_WIIU
    WIIU_LOG("entering AgbMain");
#endif
    AgbMain();

#ifdef PLATFORM_WIIU
    WIIU_LOG("AgbMain returned");
#endif

    return 0;
}

// called once per GBA frame from the emulation core.
void VBlankIntrWait(void)
{
#define HANDLE_VBLANK_INTRS()                                                                                                              \
    ({                                                                                                                                     \
        REG_DISPSTAT |= INTR_FLAG_VBLANK;                                                                                                  \
        RunDMAs(DMA_VBLANK);                                                                                                               \
        if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)                                                                                           \
            gIntrTable[INTR_INDEX_VBLANK]();                                                                                               \
        REG_DISPSTAT &= ~INTR_FLAG_VBLANK;                                                                                                 \
    })

    if (headless) {
        REG_VCOUNT = DISPLAY_HEIGHT + 1;
        HANDLE_VBLANK_INTRS();
        return;
    }

#ifdef PLATFORM_WIIU
    if (!sWiiUVBlankLogged) {
        sWiiUVBlankLogged = true;
        WIIU_LOG("enter VBlankIntrWait");
    }
#endif

#ifndef __PSP__
    ProcessSDLEvents();
#endif

    if (!isRunning) {
        CloseSaveFile();
#ifdef PLATFORM_WIIU
        WIIU_LOG("shutting down after main loop");
#endif
#if defined(USE_PLATFORM_RENDERER)
        GpuRenderer_Shutdown();
#elif defined(PLATFORM_WIIU)
        GX2Present_Shutdown();
#endif
        SDL_DestroyWindow(sdlWindow);
        SDL_Quit();
#ifdef __PSP__
        sceKernelExitGame();
#endif
        exit(0);
    }

    if (!paused || stepOneFrame) {
        REG_KEYINPUT = KEYS_MASK ^ Platform_GetKeyInput();
#if defined(USE_PLATFORM_RENDERER)
        REG_VCOUNT = DISPLAY_HEIGHT + 1; // prep for being in VBlank period
#else
        VDraw();
#endif
        HANDLE_VBLANK_INTRS();

        if (paused && stepOneFrame) {
            stepOneFrame = false;
        }
    }

    // present
#ifdef __PSP__
    // manual blit since SDL_RenderSetLogicalSize doesn't work on psp
    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &pspDestRect);
    SDL_RenderPresent(sdlRenderer);
#elif defined(USE_PLATFORM_RENDERER)
    if (videoScaleChanged) {
        SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
        videoScaleChanged = false;
    }
#elif defined(PLATFORM_WIIU)
    GX2Present_Present();
    sWiiURenderFrameCounter++;
#else
    SDL_RenderClear(sdlRenderer);
    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);

#if ENABLE_VRAM_VIEW
    VramDraw(vramTexture);
    SDL_RenderClear(vramRenderer);
    SDL_RenderCopy(vramRenderer, vramTexture, NULL, NULL);
#endif
    if (videoScaleChanged) {
        SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
        videoScaleChanged = false;
    }

#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiURenderFrameCounter)) {
        WIIU_LOG("frame[%u] SDL_RenderPresent begin", sWiiURenderFrameCounter);
    }
#endif
    SDL_RenderPresent(sdlRenderer);
#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiURenderFrameCounter)) {
        WIIU_LOG("frame[%u] SDL_RenderPresent end", sWiiURenderFrameCounter);
    }
    sWiiURenderFrameCounter++;
#endif
#if ENABLE_VRAM_VIEW
    SDL_RenderPresent(vramRenderer);
#endif
#endif
#undef HANDLE_VBLANK_INTRS
}

static void ReadSaveFile(char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL) {
        sSaveFile = fopen(path, "w+b");
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++) {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL) {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
    }
}

void Platform_StoreSaveFile(void) { StoreSaveFile(); }

static void CloseSaveFile()
{
    if (sSaveFile != NULL) {
        fclose(sSaveFile);
    }
}

static u16 keys;

// Key mappings
#define KEY_A_BUTTON      SDLK_c
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_s
#define KEY_R_BUTTON      SDLK_d
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key)                                                                                                                  \
    case KEY_##key:                                                                                                                        \
        keys &= ~key;                                                                                                                      \
        break;

#define HANDLE_KEYDOWN(key)                                                                                                                \
    case KEY_##key:                                                                                                                        \
        keys |= key;                                                                                                                       \
        break;

#if defined(__PSP__) || defined(PLATFORM_WIIU)
#ifdef __PSP__
#define BTN_TRIANGLE 0
#define BTN_CIRCLE   1
#define BTN_CROSS    2
#define BTN_SQUARE   3
#define BTN_LTRIGGER 4
#define BTN_RTRIGGER 5
#define BTN_DOWN     6
#define BTN_LEFT     7
#define BTN_UP       8
#define BTN_RIGHT    9
#define BTN_SELECT   10
#define BTN_START    11
#endif

static u16 PollJoystickButtons(void)
{
    u16 newKeys = 0;

#ifdef __PSP__
    if (joystick == NULL)
        return newKeys;

    SDL_JoystickUpdate();

    if (SDL_JoystickGetButton(joystick, BTN_CROSS))
        newKeys |= A_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_CIRCLE))
        newKeys |= B_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_SQUARE))
        newKeys |= B_BUTTON; // Square also B
    if (SDL_JoystickGetButton(joystick, BTN_START))
        newKeys |= START_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_SELECT))
        newKeys |= SELECT_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_LTRIGGER))
        newKeys |= L_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_RTRIGGER))
        newKeys |= R_BUTTON;
    if (SDL_JoystickGetButton(joystick, BTN_UP))
        newKeys |= DPAD_UP;
    if (SDL_JoystickGetButton(joystick, BTN_DOWN))
        newKeys |= DPAD_DOWN;
    if (SDL_JoystickGetButton(joystick, BTN_LEFT))
        newKeys |= DPAD_LEFT;
    if (SDL_JoystickGetButton(joystick, BTN_RIGHT))
        newKeys |= DPAD_RIGHT;
#endif

#ifdef PLATFORM_WIIU
    SDL_GameControllerUpdate();
    SDL_JoystickUpdate();

    for (int ci = 0; ci < sWiiUNumControllers; ci++) {
        WiiUControllerSlot *slot = &sWiiUControllers[ci];
        SDL_GameController *ctrl = slot->controller;
        SDL_Joystick *joy = slot->joystick;

        if (slot->isGameController) {
            if (ctrl == NULL || !SDL_GameControllerGetAttached(ctrl))
                continue;

            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_B))
                newKeys |= A_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_A))
                newKeys |= B_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_START))
                newKeys |= START_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_BACK))
                newKeys |= SELECT_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
                newKeys |= L_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
                newKeys |= R_BUTTON;
            if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192)
                newKeys |= L_BUTTON;
            if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192)
                newKeys |= R_BUTTON;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP)
                || SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY) < -WIIU_AXIS_DEADZONE)
                newKeys |= DPAD_UP;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                || SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY) > WIIU_AXIS_DEADZONE)
                newKeys |= DPAD_DOWN;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT)
                || SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX) < -WIIU_AXIS_DEADZONE)
                newKeys |= DPAD_LEFT;
            if (SDL_GameControllerGetButton(ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
                || SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX) > WIIU_AXIS_DEADZONE)
                newKeys |= DPAD_RIGHT;
        } else if (joy != NULL && SDL_JoystickGetAttached(joy)) {
            int numButtons = SDL_JoystickNumButtons(joy);
            int numAxes = SDL_JoystickNumAxes(joy);
            int numHats = SDL_JoystickNumHats(joy);

            if (numButtons > 0 && SDL_JoystickGetButton(joy, 0))
                newKeys |= A_BUTTON;
            if (numButtons > 1 && SDL_JoystickGetButton(joy, 1))
                newKeys |= B_BUTTON;
            if (numButtons > 4 && SDL_JoystickGetButton(joy, 4))
                newKeys |= L_BUTTON;
            if (numButtons > 5 && SDL_JoystickGetButton(joy, 5))
                newKeys |= R_BUTTON;
            if (numButtons > 6 && SDL_JoystickGetButton(joy, 6))
                newKeys |= SELECT_BUTTON;
            if (numButtons > 7 && SDL_JoystickGetButton(joy, 7))
                newKeys |= START_BUTTON;

            if (numAxes > 0) {
                Sint16 x = SDL_JoystickGetAxis(joy, 0);
                if (x < -WIIU_AXIS_DEADZONE)
                    newKeys |= DPAD_LEFT;
                if (x > WIIU_AXIS_DEADZONE)
                    newKeys |= DPAD_RIGHT;
            }
            if (numAxes > 1) {
                Sint16 y = SDL_JoystickGetAxis(joy, 1);
                if (y < -WIIU_AXIS_DEADZONE)
                    newKeys |= DPAD_UP;
                if (y > WIIU_AXIS_DEADZONE)
                    newKeys |= DPAD_DOWN;
            }
            for (int hatIndex = 0; hatIndex < numHats; hatIndex++) {
                Uint8 hat = SDL_JoystickGetHat(joy, hatIndex);
                if (hat & SDL_HAT_UP)
                    newKeys |= DPAD_UP;
                if (hat & SDL_HAT_DOWN)
                    newKeys |= DPAD_DOWN;
                if (hat & SDL_HAT_LEFT)
                    newKeys |= DPAD_LEFT;
                if (hat & SDL_HAT_RIGHT)
                    newKeys |= DPAD_RIGHT;
            }
        }
    }

    if (newKeys != sWiiULastControllerKeys) {
        WIIU_LOG("controller keys changed old=0x%04X new=0x%04X", sWiiULastControllerKeys, newKeys);
        sWiiULastControllerKeys = newKeys;
    }
#endif

    return newKeys;
}
#endif

u32 fullScreenFlags = 0;
static SDL_DisplayMode sdlDispMode = { 0 };

void Platform_QueueAudio(const s16 *data, uint32_t bytesCount)
{
    if (headless) {
        return;
    }

#if defined(PLATFORM_WIIU)
    if (!sWiiUAudioEnabled) {
        return;
    }

    LockAudioPlayback();

    if (WiiUShouldTraceCounter(sWiiUAudioQueueCounter)) {
        WiiUAudioStats stats = WiiUAudioGatherStats(data, bytesCount);
        WIIU_LOG("audio[%u] push bytes=%u samples=%u buffered_before=%u cb=%u underflows=%u dropped=%u min=%d max=%d peak=%u nonzero=%u first=%d,%d,%d,%d",
                 sWiiUAudioQueueCounter, bytesCount, stats.sampleCount, (u32)sWiiUAudioBufferedBytes,
                 sWiiUAudioCallbackCounter, sWiiUAudioUnderflowCounter, sWiiUAudioDroppedBytes,
                 stats.minSample, stats.maxSample, stats.peak, stats.nonzeroCount,
                 stats.firstSamples[0], stats.firstSamples[1], stats.firstSamples[2], stats.firstSamples[3]);
    }

    WiiUAudioPush((const Uint8 *)data, bytesCount);

    if (WiiUShouldTraceCounter(sWiiUAudioQueueCounter)) {
        WIIU_LOG("audio[%u] after push buffered=%u", sWiiUAudioQueueCounter, (u32)sWiiUAudioBufferedBytes);
    }

    UnlockAudioPlayback();
    sWiiUAudioQueueCounter++;
    return;
#endif

    Uint32 queuedAudioSize = GetQueuedAudioPlaybackSize();

#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiUAudioQueueCounter)) {
        WIIU_LOG("audio[%u] before queue bytes=%u queued=%u", sWiiUAudioQueueCounter, bytesCount, queuedAudioSize);
    }
#endif

    // Reset the audio buffer if we are 10 frames out of sync
    // If this happens it suggests there was some OS level lag
    // in playing audio. The queue length should remain stable at < 10 otherwise
    if (queuedAudioSize > (bytesCount * 10)) {
#ifdef PLATFORM_WIIU
        WIIU_LOG("audio[%u] clearing queue queued=%u limit=%u", sWiiUAudioQueueCounter, queuedAudioSize, bytesCount * 10);
#endif
        ClearQueuedAudioPlayback();
    }

    if (sdlAudioDevice != 0) {
        SDL_QueueAudio(sdlAudioDevice, data, bytesCount);
    }
#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiUAudioQueueCounter)) {
        WIIU_LOG("audio[%u] after queue queued=%u", sWiiUAudioQueueCounter, GetQueuedAudioPlaybackSize());
    }
    sWiiUAudioQueueCounter++;
#endif
    // printf("Queueing %d\n, QueueSize %d\n", bytesCount, GetQueuedAudioPlaybackSize());
}

void ProcessSDLEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
        SDL_Keycode keyCode = event.key.keysym.sym;
        Uint16 keyMod = event.key.keysym.mod;

        switch (event.type) {
            case SDL_QUIT:
#ifdef PLATFORM_WIIU
                WIIU_LOG("received SDL_QUIT event");
#endif
                isRunning = false;
                break;
#ifdef PLATFORM_WIIU
            case SDL_CONTROLLERDEVICEADDED:
                WiiUOpenController(event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                WiiUCloseController(event.cdevice.which);
                break;
            case SDL_JOYDEVICEADDED:
                if (!SDL_IsGameController(event.jdevice.which)) {
                    WiiUOpenController(event.jdevice.which);
                }
                break;
            case SDL_JOYDEVICEREMOVED:
                WiiUCloseController(event.jdevice.which);
                break;
#endif
            case SDL_KEYUP:
                switch (event.key.keysym.sym) {
                    HANDLE_KEYUP(A_BUTTON)
                    HANDLE_KEYUP(B_BUTTON)
                    HANDLE_KEYUP(START_BUTTON)
                    HANDLE_KEYUP(SELECT_BUTTON)
                    HANDLE_KEYUP(L_BUTTON)
                    HANDLE_KEYUP(R_BUTTON)
                    HANDLE_KEYUP(DPAD_UP)
                    HANDLE_KEYUP(DPAD_DOWN)
                    HANDLE_KEYUP(DPAD_LEFT)
                    HANDLE_KEYUP(DPAD_RIGHT)
                    case SDLK_SPACE:
                        if (speedUp) {
                            speedUp = false;
                            timeScale = 1.0;
#if defined(PLATFORM_WIIU) && ENABLE_AUDIO
                            LockAudioPlayback();
                            WiiUAudioReset();
                            UnlockAudioPlayback();
#else
                            ClearQueuedAudioPlayback();
#endif
                            PauseAudioPlayback(0);
                        }
                        break;
                }
                break;
            case SDL_KEYDOWN:
                if (keyCode == SDLK_RETURN && (keyMod & KMOD_ALT)) {
                    fullScreenFlags ^= SDL_WINDOW_FULLSCREEN_DESKTOP;
                    if (fullScreenFlags & SDL_WINDOW_FULLSCREEN_DESKTOP) {
                        SDL_GetWindowDisplayMode(sdlWindow, &sdlDispMode);
                        preFullscreenVideoScale = videoScale;
                    } else {
                        SDL_SetWindowDisplayMode(sdlWindow, &sdlDispMode);
                        videoScale = preFullscreenVideoScale;
                    }
                    SDL_SetWindowFullscreen(sdlWindow, fullScreenFlags);

                    SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                    videoScaleChanged = FALSE;
                } else
                    switch (event.key.keysym.sym) {
                        HANDLE_KEYDOWN(A_BUTTON)
                        HANDLE_KEYDOWN(B_BUTTON)
                        HANDLE_KEYDOWN(START_BUTTON)
                        HANDLE_KEYDOWN(SELECT_BUTTON)
                        HANDLE_KEYDOWN(L_BUTTON)
                        HANDLE_KEYDOWN(R_BUTTON)
                        HANDLE_KEYDOWN(DPAD_UP)
                        HANDLE_KEYDOWN(DPAD_DOWN)
                        HANDLE_KEYDOWN(DPAD_LEFT)
                        HANDLE_KEYDOWN(DPAD_RIGHT)
                        case SDLK_r:
                            if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) {
                                DoSoftReset();
                            }
                            break;
                        case SDLK_p:
                            if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) {
                                paused = !paused;
                            }
                            break;
                        case SDLK_SPACE:
                            if (!speedUp) {
                                speedUp = true;
                                timeScale = SPEEDUP_SCALE;
                                PauseAudioPlayback(1);
                            }
                            break;
                        case SDLK_F10:
                            paused = true;
                            stepOneFrame = true;
                            break;
                    }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    unsigned int w = event.window.data1;
                    unsigned int h = event.window.data2;

                    videoScale = 0;
                    if (w / DISPLAY_WIDTH > videoScale)
                        videoScale = w / DISPLAY_WIDTH;
                    if (h / DISPLAY_HEIGHT > videoScale)
                        videoScale = h / DISPLAY_HEIGHT;
                    if (videoScale < 1)
                        videoScale = 1;

                    videoScaleChanged = true;

#ifdef PLATFORM_WIIU
                    WIIU_LOG("window size changed to %ux%u videoScale=%u", w, h, videoScale);
#endif
                }
                break;
        }
    }
}

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    SharedKeys gamepadKeys = GetXInputKeys();

    speedUp = (gamepadKeys & KEY_SPEEDUP) ? true : false;

    if (speedUp) {
        timeScale = SPEEDUP_SCALE;
        PauseAudioPlayback(1);
    } else {
        timeScale = 1.0f;
        PauseAudioPlayback(0);
    }

    return (gamepadKeys != 0) ? gamepadKeys : keys;
#endif

#if defined(__PSP__) || defined(PLATFORM_WIIU)
    return keys | PollJoystickButtons();
#endif

    return keys;
}

#if ENABLE_VRAM_VIEW
void VramDraw(SDL_Texture *texture)
{
    memset(vramBuffer, 0, sizeof(vramBuffer));
    gpsp_draw_vram_view(vramBuffer);
    SDL_UpdateTexture(texture, NULL, vramBuffer, VRAM_VIEW_WIDTH * sizeof(Uint16));
}
#endif

#if !defined(USE_PLATFORM_RENDERER)
void VDraw(void)
{
#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiURenderFrameCounter)) {
        WIIU_LOG("frame[%u] VDraw begin", sWiiURenderFrameCounter);
    }
#endif
    gpsp_draw_frame(gameImage);
#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiURenderFrameCounter)) {
        WIIU_LOG("frame[%u] gpsp_draw_frame done", sWiiURenderFrameCounter);
    }
#endif
    UploadGameTexture();
#ifdef PLATFORM_WIIU
    if (WiiUShouldTraceCounter(sWiiURenderFrameCounter)) {
        WIIU_LOG("frame[%u] VDraw end", sWiiURenderFrameCounter);
    }
#endif
    REG_VCOUNT = DISPLAY_HEIGHT + 1; // prep for being in VBlank period
}

static void UploadGameTexture(void)
{
#if defined(PLATFORM_WIIU)
    if (!GX2Present_UploadFrame(gameImage))
        WIIU_LOG("frame[%u] GX2Present_UploadFrame failed: %s", sWiiURenderFrameCounter, SDL_GetError());
#else
    CopyFrameToTextureBuffer(textureUploadImage, gameImage);
    SDL_UpdateTexture(sdlTexture, NULL, textureUploadImage, DISPLAY_WIDTH * sizeof(Uint16));
#endif
}
#endif

#if defined(USE_PLATFORM_RENDERER)
void Platform_ProcessBackgroundsCopyQueue(void) { GpuRenderer_ProcessBackgroundsCopyQueue(); }

void Platform_TransformSprite(Sprite *sprite, SpriteTransform *transform) { GpuRenderer_TransformSprite(sprite, transform); }

void Platform_DisplaySprite(Sprite *sprite, u8 oamPaletteNum) { GpuRenderer_DisplaySprite(sprite, oamPaletteNum); }
#endif
