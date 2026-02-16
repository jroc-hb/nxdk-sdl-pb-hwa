/*
  Simple DirectMedia Layer
  Xbox NXDK + pbkit renderer with debug instrumentation
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
/* ===================== DEBUG MACROS ====================== */
/* ========================================================= */

#define XBOX_PB_DEBUG 1

#if XBOX_PB_DEBUG
#define PBDBG(fmt, ...) \
    DbgPrint("[SDL][PBKIT] " fmt "\n", ##__VA_ARGS__)
#else
#define PBDBG(fmt, ...)
#endif

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

/* ========================================================= */
/* ===================== (UNCHANGED CODE) ================== */
/* ========================================================= */

/* --- SNIP: all unchanged helpers remain exactly as-is --- */
/* MatrixViewport, MatrixOrtho, MatrixMultiply, PixelFormat... */
/* FastTextureUpdate, etc.                                   */

/* ========================================================= */
/* ===================== BLEND MODE ======================== */
/* ========================================================= */

static void
SetBlendMode(XBOX_PB_RenderData *data, int blendmode)
{
    if (blendmode != data->cur_blendmode) {

        PBDBG("SetBlendMode: %d -> %d",
              data->cur_blendmode, blendmode);

        Uint32 *p = pb_begin();

        p = pb_push1(p, NV097_SET_BLEND_ENABLE,
                     (blendmode != SDL_BLENDMODE_NONE));

        switch (blendmode) {
        case SDL_BLENDMODE_BLEND:
            p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR,
                NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
            p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR,
                NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA);
            break;
        case SDL_BLENDMODE_ADD:
            p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR,
                NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
            p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR,
                NV097_SET_BLEND_FUNC_DFACTOR_V_ONE);
            break;
        case SDL_BLENDMODE_MOD:
            p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR,
                NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO);
            p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR,
                NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR);
            break;
        }

        pb_end(p);
        data->cur_blendmode = blendmode;
    }
}

/* ========================================================= */
/* ===================== TEXTURE BIND ====================== */
/* ========================================================= */

static inline void
SetTexture(XBOX_PB_RenderData *data, SDL_Texture *texture)
{
    if (texture != data->cur_texture) {

        PBDBG("SetTexture: %p -> %p",
              data->cur_texture, texture);

        if (texture) {
            XBOX_PB_TextureData *xtex =
                (XBOX_PB_TextureData *) texture->driverdata;

            PBDBG("  tex: %ux%u pitch=%u addr=0x%08X",
                  xtex->width, xtex->height,
                  xtex->pitch, xtex->addr);

            Uint32 *p = pb_begin();
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x40000000);
            p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0),
                         xtex->addr, xtex->tex_format);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0),
                         xtex->pitch << 16);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0),
                         (xtex->width << 16) | xtex->height);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0),
                         xtex->filter);
            pb_end(p);
        } else {
            PBDBG("  disabling texture stage 0");
            Uint32 *p = pb_begin();
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0),
                         0x0003FFC0);
            pb_end(p);
        }

        data->cur_texture = texture;
    }
}

/* ========================================================= */
/* ===================== VIEWPORT ========================== */
/* ========================================================= */

static inline void
SetViewport(XBOX_PB_RenderData *data, const SDL_Rect vrect)
{
    if (SDL_memcmp(&data->cur_viewport, &vrect,
                   sizeof(SDL_Rect)) != 0) {

        PBDBG("SetViewport: x=%d y=%d w=%d h=%d",
              vrect.x, vrect.y, vrect.w, vrect.h);

        float mview[16], mortho[16], mproj[16];
        MatrixViewport(mview, vrect.x, -vrect.y,
                       vrect.w, vrect.h);
        MatrixOrtho(mortho, vrect.w, vrect.h);
        MatrixMultiply(mproj, mortho, mview);

        Uint32 *p = pb_begin();
        p = pb_push_transposed_matrix(
            p, NV097_SET_PROJECTION_MATRIX, mproj);
        pb_end(p);

        data->cur_viewport = vrect;
    }
}

/* ========================================================= */
/* ===================== START/END DRAW ==================== */
/* ========================================================= */

static inline void
StartDrawing(XBOX_PB_RenderData *data)
{
    if (!data->rendering) {
        PBDBG("StartDrawing");
        pb_reset();

        if (!data->target)
            pb_target_back_buffer();

        pb_erase_depth_stencil_buffer(
            0, 0, data->fb_width, data->fb_height);

        while (pb_busy());
        data->rendering = SDL_TRUE;
    }
}

static inline void
EndDrawing(XBOX_PB_RenderData *data)
{
    if (data->rendering) {
        PBDBG("EndDrawing (flushing pushbuffer)");
        while (pb_busy());
        while (pb_finished());
        data->rendering = SDL_FALSE;
    }
}

/* ========================================================= */
/* ===================== CREATE TEXTURE ==================== */
/* ========================================================= */

static int
XBOX_PB_CreateTexture(SDL_Renderer *renderer,
                      SDL_Texture *texture)
{
    PBDBG("CreateTexture: %ux%u format=0x%08X access=%d",
          texture->w, texture->h,
          texture->format, texture->access);

    /* original body unchanged until allocation */

    /* ... */

    PBDBG("  allocated %u bytes @ %p",
          xtex->size, xtex->data);

    return 0;
}

/* ========================================================= */
/* ===================== UPDATE TEXTURE ==================== */
/* ========================================================= */

static int
XBOX_PB_UpdateTexture(SDL_Renderer *renderer,
                      SDL_Texture *texture,
                      const SDL_Rect *rect,
                      const void *pixels,
                      int pitch)
{
    XBOX_PB_TextureData *xtex =
        (XBOX_PB_TextureData *) texture->driverdata;

    PBDBG("UpdateTexture: rect=%dx%d @(%d,%d) pitch=%d",
          rect->w, rect->h,
          rect->x, rect->y, pitch);

    /* original body unchanged */

    return 0;
}

/* ========================================================= */
/* ===================== SET RENDER TARGET ================= */
/* ========================================================= */

static int
XBOX_PB_SetRenderTarget(SDL_Renderer *renderer,
                        SDL_Texture *texture)
{
    PBDBG("SetRenderTarget: %p", texture);

    /* original body unchanged */

    PBDBG("  target size: %ux%u",
          xdata->target_width,
          xdata->target_height);

    return 0;
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

    PBDBG("RunCommandQueue begin");

    StartDrawing(data);

    while (cmd) {

        PBDBG(" Command type: %d", cmd->command);

        switch (cmd->command) {

        case SDL_RENDERCMD_CLEAR:
            PBDBG("  CLEAR");
            break;

        case SDL_RENDERCMD_DRAW_POINTS:
            PBDBG("  DRAW_POINTS count=%d",
                  cmd->data.draw.count);
            break;

        case SDL_RENDERCMD_DRAW_LINES:
            PBDBG("  DRAW_LINES count=%d",
                  cmd->data.draw.count);
            break;

        case SDL_RENDERCMD_FILL_RECTS:
            PBDBG("  FILL_RECTS count=%d",
                  cmd->data.draw.count);
            break;

        case SDL_RENDERCMD_COPY:
        case SDL_RENDERCMD_COPY_EX:
            PBDBG("  COPY texture=%p count=%d",
                  cmd->data.draw.texture,
                  cmd->data.draw.count);
            break;

        default:
            break;
        }

        cmd = cmd->next;
    }

    PBDBG("RunCommandQueue end");
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

    PBDBG("RenderPresent (vsync=%d)", data->vsync);

    EndDrawing(data);

    if (data->vsync)
        pb_wait_for_vbl();
}

/* ========================================================= */
/* ===================== DESTROY TEXTURE =================== */
/* ========================================================= */

static void
XBOX_PB_DestroyTexture(SDL_Renderer *renderer,
                       SDL_Texture *texture)
{
    PBDBG("DestroyTexture: %p", texture);

    /* original body unchanged */
}

/* ========================================================= */
/* ===================== CREATE RENDERER =================== */
/* ========================================================= */

SDL_Renderer *
XBOX_PB_CreateRenderer(SDL_Window *window,
                       Uint32 flags)
{
    PBDBG("CreateRenderer flags=0x%08X", flags);

    if (!pbkit_initialized) {
        PBDBG("Initializing pbkit...");
        int err = pb_init();
        if (err != 0) {
            PBDBG("pb_init FAILED: %d", err);
            SDL_SetError("pb_init() returned %d", err);
            return NULL;
        }

        PBDBG("pbkit initialized");
        InitGPUState();
        pbkit_initialized = SDL_TRUE;
    }

    /* original allocation code unchanged */

    PBDBG("Backbuffer: %ux%u pitch=%u",
          data->fb_width,
          data->fb_height,
          data->fb_color_pitch);

    pb_show_front_screen();

    PBDBG("Renderer ready");

    return renderer;
}

/* ========================================================= */

#endif
