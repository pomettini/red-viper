// Playdate-side bridge from the Red Viper software VIP renderer
// (source/common/video_soft.cpp) to the Playdate 1-bit display.
//
// Flow per emulated VB frame:
//   1. If XPEN is set, run update_texture_cache_soft (if CharCacheInvalid)
//      and video_soft_render(drawn_fb). The renderer writes a 384x224 2bpp
//      column-major framebuffer into V810_DISPLAY_RAM at the appropriate
//      offset.
//   2. Read the displayed framebuffer (left eye only — VB is stereo, the
//      Playdate display isn't), threshold-convert 2bpp to 1bpp, and blit
//      it centered into the Playdate's 400x240 frame.
//
// VB framebuffer layout (single eye, single buffer, 384x224 visible):
//   - Column-major. Each column is 256 rows × 2 bpp = 64 bytes.
//   - In each byte, bits [1:0] = row r, [3:2] = r+1, [5:4] = r+2, [7:6] = r+3.
//   - Value 0 = transparent/black, 1/2/3 = three brightness levels.
//
// Playdate framebuffer layout: 400 cols × 240 rows, 1 bpp, MSB-first per byte,
// row stride is LCD_ROWSIZE bytes (52). Bit set = white.
//
// Centering: VB 384x224 → Playdate 400x240 with 8 px horizontal and 8 px
// vertical margin (see NOTES.md §6).

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pd_api.h"
#include "pd_video.h"

#include "vb_types.h"
#include "vb_set.h"
#include "vb_dsp.h"
#include "v810_mem.h"

extern void update_texture_cache_soft(void);
extern void video_soft_render(int drawn_fb);

#define VB_W            384
#define VB_H            224
#define PD_W            400
#define PD_H            240
#define PD_STRIDE       52    // LCD_ROWSIZE
#define MARGIN_X        ((PD_W - VB_W) / 2)   // 8
#define MARGIN_Y        ((PD_H - VB_H) / 2)   // 8
#define THRESHOLD       2     // VB value >= threshold => Playdate white

void pd_video_vip_step(PlaydateAPI *pd) {
    if (!vb_state) return;

    int displayed_fb = vb_state->tVIPREG.tDisplayedFB & 1;
    int drawn_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : 0;

    if (vb_state->tVIPREG.XPCTRL & XPEN) {
        // Split-timing instrumentation: find where vip's cost goes (tile-cache
        // rebuild vs world/object compositing) before optimising. ms-resolution
        // is coarse but enough to see the dominant part. Logged every 60 frames.
        unsigned t0 = pd->system->getCurrentTimeMilliseconds();
        if (tDSPCACHE.CharCacheInvalid) {
            update_texture_cache_soft();
        }
        unsigned t1 = pd->system->getCurrentTimeMilliseconds();
        video_soft_render(drawn_fb);
        unsigned t2 = pd->system->getCurrentTimeMilliseconds();

        static unsigned acc_cache, acc_render, n;
        acc_cache += (t1 - t0);
        acc_render += (t2 - t1);
        if (++n >= 60) {
            pd->system->logToConsole("vipsplit: cache=%u render=%u ms (avg over %u)",
                                     acc_cache / n, acc_render / n, n);
            acc_cache = acc_render = n = 0;
        }

        // The original renderer-driver in source/common/video.c clears these
        // after a render pass; we keep the same convention so subsequent
        // frames don't redo cached work.
        tDSPCACHE.CharCacheInvalid = false;
        memset(tDSPCACHE.BGCacheInvalid, 0, sizeof(tDSPCACHE.BGCacheInvalid));
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }
}

void pd_video_blit(PlaydateAPI *pd) {
    if (!vb_state) return;

    int displayed_fb = vb_state->tVIPREG.tDisplayedFB & 1;
    int drawn_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : 0;
    int read_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : drawn_fb;
    const uint8_t *vbfb =
        (const uint8_t *)(vb_state->V810_DISPLAY_RAM.off + 0x8000 * read_fb);

    uint8_t *pdfb = pd->graphics->getFrame();
    if (!pdfb) return;

    // Clear all rows to black; we only OR in the VB-bright pixels.
    memset(pdfb, 0x00, PD_H * PD_STRIDE);

    // Iterate by VB column, not by Playdate row, so the SDRAM reads are
    // sequential within each 64-byte VB column. Each column lives in two
    // 32-byte cache lines, so ~2 cache misses per column instead of one
    // miss per pixel — drops total misses from ~86k to ~768.
    //
    // Each VB byte holds 4 consecutive rows of one column, so one read
    // produces four scattered writes into the Playdate framebuffer.
    // Playdate FB sits in fast (cached) RAM so the scatter is cheap.
    for (int vbc = 0; vbc < VB_W; vbc++) {
        const uint8_t *col = vbfb + vbc * 64;
        int px = vbc + MARGIN_X;
        int pdb_off = px >> 3;
        uint8_t pdmask = (uint8_t)(0x80 >> (px & 7));

        for (int bi = 0; bi < (VB_H >> 2); bi++) {
            uint8_t b = col[bi];
            if (b == 0) continue;  // common case: empty column slice
            uint8_t *r = pdfb + (MARGIN_Y + (bi << 2)) * PD_STRIDE + pdb_off;
            if (((b     ) & 3) >= THRESHOLD) r[0 * PD_STRIDE] |= pdmask;
            if (((b >> 2) & 3) >= THRESHOLD) r[1 * PD_STRIDE] |= pdmask;
            if (((b >> 4) & 3) >= THRESHOLD) r[2 * PD_STRIDE] |= pdmask;
            if (((b >> 6) & 3) >= THRESHOLD) r[3 * PD_STRIDE] |= pdmask;
        }
    }

    pd->graphics->markUpdatedRows(MARGIN_Y, MARGIN_Y + VB_H - 1);
}
