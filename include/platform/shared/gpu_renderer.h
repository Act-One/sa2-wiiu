#ifndef GUARD_PLATFORM_SHARED_GPU_RENDERER_H
#define GUARD_PLATFORM_SHARED_GPU_RENDERER_H

#include <stdbool.h>

#include <SDL.h>

#include "sprite.h"

bool GpuRenderer_Init(SDL_Window *window);
void GpuRenderer_Shutdown(void);
void GpuRenderer_ProcessBackgroundsCopyQueue(void);
void GpuRenderer_TransformSprite(Sprite *sprite, SpriteTransform *transform);
void GpuRenderer_DisplaySprite(Sprite *sprite, u8 oamPaletteNum);

#endif // GUARD_PLATFORM_SHARED_GPU_RENDERER_H
