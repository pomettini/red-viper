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
extern void video_soft_render_1bpp(void);
extern uint8_t pd_render_fb1[];   // 1bpp column-major, 32 bytes/column

#define RV_FB1_STRIDE 32

#define VB_W            384
#define VB_H            224
#define PD_W            400
#define PD_H            240
#define PD_STRIDE       52    // LCD_ROWSIZE
#define MARGIN_X        ((PD_W - VB_W) / 2)   // 8
#define MARGIN_Y        ((PD_H - VB_H) / 2)   // 8
#define THRESHOLD       2     // VB value >= threshold => Playdate white

void pd_video_vip_step(PlaydateAPI *pd) {
    (void)pd;
    if (!vb_state) return;

    if (vb_state->tVIPREG.XPCTRL & XPEN) {
        if (tDSPCACHE.CharCacheInvalid) {
            update_texture_cache_soft();
        }

        video_soft_render_1bpp();

        // The original renderer-driver in source/common/video.c clears these
        // after a render pass; we keep the same convention so subsequent
        // frames don't redo cached work.
        tDSPCACHE.CharCacheInvalid = false;
        memset(tDSPCACHE.BGCacheInvalid, 0, sizeof(tDSPCACHE.BGCacheInvalid));
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }
}

// Starts true so the very first emulator frame refreshes the whole panel
// (whatever was on screen before — picker, launcher transition — gets cleared).
static bool s_full_redraw = true;

void pd_video_request_full_redraw(void) {
    s_full_redraw = true;
}

void pd_video_blit(PlaydateAPI *pd) {
    if (!vb_state) return;

    // The renderer already composited directly to 1bpp in pd_render_fb1, so the
    // blit just transposes the column-major VB layout into the row-major
    // Playdate framebuffer — no threshold/shade work left.
    const uint8_t *vbfb = pd_render_fb1;

    uint8_t *pdfb = pd->graphics->getFrame();
    if (!pdfb) return;

    // Clear all rows to black; we only OR in the bright pixels.
    memset(pdfb, 0x00, PD_H * PD_STRIDE);

    // Iterate by VB column so the reads are sequential within each 32-byte VB
    // column. Each byte holds 8 consecutive rows of one column (bit r = row r),
    // producing up to eight scattered writes into the Playdate framebuffer.
    for (int vbc = 0; vbc < VB_W; vbc++) {
        const uint8_t *col = vbfb + vbc * RV_FB1_STRIDE;
        int px = vbc + MARGIN_X;
        int pdb_off = px >> 3;
        uint8_t pdmask = (uint8_t)(0x80 >> (px & 7));

        for (int bi = 0; bi < (VB_H >> 3); bi++) {
            uint8_t b = col[bi];
            if (b == 0) continue;  // common case: empty column slice
            uint8_t *r = pdfb + (MARGIN_Y + (bi << 3)) * PD_STRIDE + pdb_off;
            if (b & 0x01) r[0 * PD_STRIDE] |= pdmask;
            if (b & 0x02) r[1 * PD_STRIDE] |= pdmask;
            if (b & 0x04) r[2 * PD_STRIDE] |= pdmask;
            if (b & 0x08) r[3 * PD_STRIDE] |= pdmask;
            if (b & 0x10) r[4 * PD_STRIDE] |= pdmask;
            if (b & 0x20) r[5 * PD_STRIDE] |= pdmask;
            if (b & 0x40) r[6 * PD_STRIDE] |= pdmask;
            if (b & 0x80) r[7 * PD_STRIDE] |= pdmask;
        }
    }

    if (s_full_redraw) {
        // Push the margin rows too (the memset above already blacked the whole
        // buffer); without this, pixels drawn by the picker/menus would stay
        // on the panel in the 8 px border forever.
        s_full_redraw = false;
        pd->graphics->markUpdatedRows(0, PD_H - 1);
    } else {
        pd->graphics->markUpdatedRows(MARGIN_Y, MARGIN_Y + VB_H - 1);
    }
}
