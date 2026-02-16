/*
  SDL Xbox PBKit Renderer (Instrumented with DbgPrint)
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_XBOX_PBKIT

#include "SDL_hints.h"
#include "SDL_assert.h"
#include "../SDL_sysrender.h"

#include <stdlib.h>
#include <math.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <x86intrin.h>
#include <xboxkrnl/xboxkrnl.h>

/* ========================================================= */
/* ===================== DEBUG SUPPORT ===================== */
/* ========================================================= */

#ifndef RENDERDBG
#define RENDERDBG 1
#endif

#if RENDERDBG
#define RDBG(fmt, ...) DbgPrint("[SDL_XBOX_PB] " fmt "\n", __VA_ARGS__)
#define RDBG0(msg)     DbgPrint("[SDL_XBOX_PB] " msg "\n")
#else
#define RDBG(fmt, ...)
#define RDBG0(msg)
#endif

/* ========================================================= */

#define PB_MAXRAM 0x03FFAFFF
#define PB_MAXZ 16777215.f

#define PB_DMA_CHANNEL_DEFAULT 9
#define PB_DMA_CHANNEL_A 3

#undef MASK
#define MASK(mask, val) (((val) << (__builtin_ffs(mask)-1)) & (mask))

#define NV097_SET_SPECULAR_ENABLE 0x000003B8
#define NV2A_VERTEX_ATTR_DIFFUSE  3
#define NV2A_VERTEX_ATTR_TEXTURE0 9
#define NV2A_VERTEX_ATTR_TEXTURE1 10

static SDL_bool pbkit_initialized = SDL_FALSE;

/* ========================================================= */
/* ================== DATA STRUCTURES ====================== */
/* ========================================================= */

typedef struct
{
    SDL_BlendMode cur_blendmode;
    SDL_Texture *cur_texture;
    SDL_Rect cur_viewport;
    SDL_Rect cur_cliprect;
    Uint32 cur_color_word;
    float cur_color[4];
    unsigned int fb_width;
    unsigned int fb_height;
    unsigned int fb_color_fmt;
    unsigned int fb_color_pitch;
    unsigned int fb_depth_fmt;
    unsigned int fb_depth_pitch;
    unsigned int target_width;
    unsigned int target_height;
    SDL_Texture *target;
    SDL_bool vsync;
    SDL_bool rendering;
} XBOX_PB_RenderData;

typedef struct
{
    unsigned int tex_format;
    unsigned int surf_format;
    unsigned int width;
    unsigned int height;
    unsigned int size;
    unsigned int pitch;
    unsigned int bytespp;
    unsigned int filter;
    unsigned int addr;
    SDL_bool is_aligned;
    void *data;
} XBOX_PB_TextureData;

/* ========================================================= */
/* ===================== GPU HELPERS ======================= */
/* ========================================================= */

static inline void InitGPUState(void)
{
    RDBG0("InitGPUState(): Initializing GPU state");

    Uint32 *p = pb_begin();

    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);

    pb_end(p);

    while (pb_busy());

    RDBG0("InitGPUState(): GPU state initialized");
}

static inline void StartDrawing(XBOX_PB_RenderData *data)
{
    if (!data->rendering) {
        RDBG("StartDrawing(): fb=%ux%u target=%p",
             data->fb_width, data->fb_height, data->target);

        pb_reset();
        if (!data->target)
            pb_target_back_buffer();

        pb_erase_depth_stencil_buffer(0, 0,
                                      data->fb_width,
                                      data->fb_height);

        while (pb_busy());
        data->rendering = SDL_TRUE;

        RDBG0("StartDrawing(): Rendering started");
    }
}

static inline void EndDrawing(XBOX_PB_RenderData *data)
{
    if (data->rendering) {
        RDBG0("EndDrawing(): Waiting for GPU");

        while (pb_busy());
        while (pb_finished());

        data->rendering = SDL_FALSE;
        RDBG0("EndDrawing(): Rendering finished");
    }
}

/* ========================================================= */
/* ===================== TEXTURES ========================== */
/* ========================================================= */

static int
XBOX_PB_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    RDBG("CreateTexture(): %ux%u format=0x%08X access=%d",
         texture->w, texture->h,
         texture->format, texture->access);

    XBOX_PB_TextureData *xtex =
        (XBOX_PB_TextureData *) SDL_calloc(1, sizeof(XBOX_PB_TextureData));

    if (!xtex) {
        RDBG0("CreateTexture(): Allocation FAILED");
        SDL_OutOfMemory();
        return -1;
    }

    xtex->width  = texture->w;
    xtex->height = texture->h;
    xtex->bytespp = SDL_BYTESPERPIXEL(texture->format);
    xtex->pitch = xtex->bytespp * xtex->width;
    xtex->size  = xtex->pitch * xtex->height;
    xtex->is_aligned = (xtex->size & 63) == 0;

    xtex->data = MmAllocateContiguousMemoryEx(
        xtex->size, 0, PB_MAXRAM, 16, 0x404);

    if (!xtex->data) {
        SDL_free(xtex);
        SDL_OutOfMemory();
        return -1;
    }

    SDL_memset(xtex->data, 0, xtex->size);

    xtex->addr = ((unsigned int)xtex->data) & 0x03FFFFFF;

    texture->driverdata = xtex;

    RDBG("CreateTexture(): Allocated %u bytes at %p pitch=%u",
         xtex->size, xtex->data, xtex->pitch);

    return 0;
}

static int
XBOX_PB_UpdateTexture(SDL_Renderer *renderer,
                      SDL_Texture *texture,
                      const SDL_Rect *rect,
                      const void *pixels,
                      int pitch)
{
    XBOX_PB_TextureData *xtex =
        (XBOX_PB_TextureData *) texture->driverdata;

    RDBG("UpdateTexture(): rect=%dx%d (%d,%d)",
         rect->w, rect->h, rect->x, rect->y);

    const Uint8 *src = pixels;
    Uint8 *dst = (Uint8 *)xtex->data +
                 rect->y * xtex->pitch +
                 rect->x * xtex->bytespp;

    size_t length = rect->w * xtex->bytespp;

    for (int row = 0; row < rect->h; row++) {
        SDL_memcpy(dst, src, length);
        src += pitch;
        dst += xtex->pitch;
    }

    return 0;
}

static void
XBOX_PB_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    RDBG("DestroyTexture(): %p", texture);

    XBOX_PB_TextureData *xtex =
        (XBOX_PB_TextureData *) texture->driverdata;

    if (!xtex)
        return;

    if (xtex->data)
        MmFreeContiguousMemory(xtex->data);

    SDL_free(xtex);
    texture->driverdata = NULL;

    RDBG0("DestroyTexture(): Freed texture");
}

/* ========================================================= */
/* ===================== COMMAND QUEUE ===================== */
/* ========================================================= */

static int
XBOX_PB_RunCommandQueue(SDL_Renderer *renderer,
                        SDL_RenderCommand *cmd,
                        void *vertices,
                        size_t vertsize)
{
    XBOX_PB_RenderData *data =
        (XBOX_PB_RenderData *) renderer->driverdata;

    RDBG0("RunCommandQueue(): Begin");

    StartDrawing(data);

    int cmd_index = 0;

    while (cmd) {
        RDBG(" Command #%d type=%d",
             cmd_index++, cmd->command);

        switch (cmd->command) {

        case SDL_RENDERCMD_CLEAR: {
            Uint32 color =
                (cmd->data.color.a << 24) |
                (cmd->data.color.r << 16) |
                (cmd->data.color.g << 8)  |
                 cmd->data.color.b;

            RDBG("  CLEAR: color=0x%08X", color);

            pb_fill(0, 0,
                    data->target_width,
                    data->target_height,
                    color);
            break;
        }

        case SDL_RENDERCMD_SETDRAWCOLOR:
            RDBG("  SETDRAWCOLOR: (%u,%u,%u,%u)",
                 cmd->data.color.r,
                 cmd->data.color.g,
                 cmd->data.color.b,
                 cmd->data.color.a);
            break;

        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
            RDBG("  COPY: texture=%p",
                 cmd->data.draw.texture);
            break;

        default:
            break;
        }

        cmd = cmd->next;
    }

    RDBG0("RunCommandQueue(): End");

    return 0;
}

/* ========================================================= */
/* ===================== PRESENT =========================== */
/* ========================================================= */

static void
XBOX_PB_RenderPresent(SDL_Renderer *renderer)
{
    XBOX_PB_RenderData *data =
        (XBOX_PB_RenderData *) renderer->driverdata;

    RDBG("RenderPresent(): vsync=%d", data->vsync);

    EndDrawing(data);

    if (data->vsync) {
        RDBG0("RenderPresent(): Waiting for VBL");
        pb_wait_for_vbl();
    }
}

/* ========================================================= */
/* ===================== RENDERER ========================== */
/* ========================================================= */

static void
XBOX_PB_DestroyRenderer(SDL_Renderer *renderer)
{
    RDBG0("DestroyRenderer()");

    XBOX_PB_RenderData *data =
        (XBOX_PB_RenderData *) renderer->driverdata;

    if (data) {
        EndDrawing(data);
        SDL_free(data);
    }

    SDL_free(renderer);
}

SDL_Renderer *
XBOX_PB_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    RDBG("CreateRenderer(): flags=0x%08X", flags);

    if (!pbkit_initialized) {
        int err = pb_init();
        if (err != 0) {
            SDL_SetError("pb_init() returned %d", err);
            return NULL;
        }

        RDBG0("pb_init(): SUCCESS");

        InitGPUState();
        pbkit_initialized = SDL_TRUE;
    }

    SDL_Renderer *renderer =
        (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));

    if (!renderer)
        return NULL;

    XBOX_PB_RenderData *data =
        (XBOX_PB_RenderData *) SDL_calloc(1, sizeof(*data));

    if (!data) {
        SDL_free(renderer);
        return NULL;
    }

    renderer->driverdata = data;
    renderer->window = window;
    renderer->RunCommandQueue = XBOX_PB_RunCommandQueue;
    renderer->RenderPresent = XBOX_PB_RenderPresent;
    renderer->DestroyRenderer = XBOX_PB_DestroyRenderer;
    renderer->CreateTexture = XBOX_PB_CreateTexture;
    renderer->UpdateTexture = XBOX_PB_UpdateTexture;
    renderer->DestroyTexture = XBOX_PB_DestroyTexture;

    data->fb_width  = pb_back_buffer_width();
    data->fb_height = pb_back_buffer_height();
    data->fb_color_pitch = pb_back_buffer_pitch();

    data->target_width  = data->fb_width;
    data->target_height = data->fb_height;

    if (flags & SDL_RENDERER_PRESENTVSYNC)
        data->vsync = SDL_TRUE;

    RDBG("Backbuffer: %ux%u pitch=%u",
         data->fb_width,
         data->fb_height,
         data->fb_color_pitch);

    pb_show_front_screen();

    RDBG0("CreateRenderer(): READY");

    return renderer;
}

SDL_RenderDriver XBOX_PB_RenderDriver = {
    .CreateRenderer = XBOX_PB_CreateRenderer,
    .info = {
        .name = "xbox_pbkit",
        .flags = SDL_RENDERER_ACCELERATED |
                 SDL_RENDERER_PRESENTVSYNC |
                 SDL_RENDERER_TARGETTEXTURE,
        .num_texture_formats = 1,
        .texture_formats = { SDL_PIXELFORMAT_ARGB8888 },
        .max_texture_width = 2048,
        .max_texture_height = 2048,
     }
};

#endif
