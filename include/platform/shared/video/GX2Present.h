#ifndef GUARD_PLATFORM_SHARED_VIDEO_GX2_PRESENT_H
#define GUARD_PLATFORM_SHARED_VIDEO_GX2_PRESENT_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

bool GX2Present_Init(SDL_Window *window, int frameWidth, int frameHeight);
void GX2Present_Shutdown(void);
bool GX2Present_UploadFrame(const uint16_t *framePixels);
void GX2Present_Present(void);

#endif // GUARD_PLATFORM_SHARED_VIDEO_GX2_PRESENT_H
