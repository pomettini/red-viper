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

void pd_video_render_frame(PlaydateAPI *pd) {
    if (!vb_state) return;

    // Decide which framebuffer the soft renderer should write to. The VB
    // double-buffer ping-pongs on every frame end; for single-buffer games
    // (most of them, including Wario Land) we just always use FB 0.
    int displayed_fb = vb_state->tVIPREG.tDisplayedFB & 1;
    int drawn_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : 0;

    if (vb_state->tVIPREG.XPCTRL & XPEN) {
        if (tDSPCACHE.CharCacheInvalid) {
            update_texture_cache_soft();
        }
        video_soft_render(drawn_fb);

        // The original renderer-driver in source/common/video.c clears these
        // after a render pass; we keep the same convention so subsequent
        // frames don't redo cached work.
        tDSPCACHE.CharCacheInvalid = false;
        memset(tDSPCACHE.BGCacheInvalid, 0, sizeof(tDSPCACHE.BGCacheInvalid));
        memset(tDSPCACHE.CharacterCache, 0, sizeof(tDSPCACHE.CharacterCache));
    }

    // Pull the left-eye, "currently displayed" framebuffer pointer.
    int read_fb = tVBOpt.DOUBLE_BUFFER ? displayed_fb : drawn_fb;
    const uint8_t *vbfb =
        (const uint8_t *)(vb_state->V810_DISPLAY_RAM.off + 0x8000 * read_fb);

    uint8_t *pdfb = pd->graphics->getFrame();
    if (!pdfb) return;

    // Clear all rows to black, then paint the VB region. Borders stay black.
    memset(pdfb, 0x00, PD_H * PD_STRIDE);

    // Per Playdate row, walk the VB columns 8 at a time, packing into one
    // output byte. VB column c stride is 64 bytes; the byte we want for VB
    // row vby is at (c * 64 + (vby >> 2)), with two bits at shift
    // ((vby & 3) << 1).
    for (int py = MARGIN_Y; py < MARGIN_Y + VB_H; py++) {
        int vby = py - MARGIN_Y;
        int byte_in_col = vby >> 2;
        int shift = (vby & 3) << 1;
        uint8_t *row = pdfb + py * PD_STRIDE;

        // Output bytes covering Playdate columns [MARGIN_X .. MARGIN_X+VB_W).
        // Compute the first and last whole bytes; handle leading/trailing
        // partial bytes separately. With MARGIN_X = 8, both edges fall on
        // byte boundaries, so just iterate per byte.
        for (int pbyte = MARGIN_X >> 3; pbyte < (MARGIN_X + VB_W) >> 3; pbyte++) {
            int vbc_base = (pbyte << 3) - MARGIN_X;
            const uint8_t *col_byte_ptr = vbfb + vbc_base * 64 + byte_in_col;
            uint8_t out = 0;
            // Bit 7 = leftmost Playdate pixel in this byte.
            if (((col_byte_ptr[0 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x80;
            if (((col_byte_ptr[1 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x40;
            if (((col_byte_ptr[2 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x20;
            if (((col_byte_ptr[3 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x10;
            if (((col_byte_ptr[4 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x08;
            if (((col_byte_ptr[5 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x04;
            if (((col_byte_ptr[6 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x02;
            if (((col_byte_ptr[7 * 64] >> shift) & 3) >= THRESHOLD) out |= 0x01;
            row[pbyte] = out;
        }
    }

    pd->graphics->markUpdatedRows(MARGIN_Y, MARGIN_Y + VB_H - 1);
}
