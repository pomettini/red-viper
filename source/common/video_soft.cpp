#include "vb_dsp.h"
#include "v810_mem.h"

// The Playdate display is 1-bit mono — there is no stereo output, so the
// right eye is composited and then thrown away by the blit. Render only the
// left eye (eye 0) there. The 3DS keeps full stereo.
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
#define SOFT_EYE_COUNT 1
// On Playdate the display is 1-bit, so we composite directly into a 1bpp
// column-major scratch buffer instead of the VB's 2bpp framebuffer. This
// halves framebuffer write traffic and, more importantly, shrinks the
// per-column tileCache working set (~4 bytes vs ~32) so it stays in the 16KB
// data cache. See NOTES.md §"1-bit renderer".
#define RV_1BPP 1
#else
#define SOFT_EYE_COUNT 2
#endif

static struct {
    // half-nibbles are colour indices
    union {
        uint16_t u16[8];
        uint32_t u32[4];
    } indices;
    // only one index, for each colour
    struct {
        struct {
            struct {
                uint16_t shade[4];
            } col[4]; // actually 3 but 4 is better aligned
        } column[8];
    } colmask;
    // mask where transparent pixels are 1
    union {
        uint16_t u16[8];
        uint32_t u32[4];
    } mask;
} tileCache[2048];

#ifdef RV_1BPP
// 1-bit-per-pixel tile cache, derived from tileCache[].indices. For each of the
// 8 columns of a tile: mask[x] has bit r set where row r is transparent
// (colour index 0); idx[x][c] has bit r set where row r has colour index c
// (c = 1..3). At render time we OR the idx[] masks whose palette-mapped shade
// is >= the white threshold to get the 8-bit bright-pixel column.
static struct {
    uint8_t mask[8];
    uint8_t idx[8][4];   // [x][0] unused
} tileCache1[2048];

// 1bpp column-major scratch framebuffer for the (left) eye: 384 columns of 32
// bytes (256 rows / 8). +64 bytes slack absorbs the once-per-column tail write
// that can index slightly past row 224 for tall worlds (matches the original
// 2bpp buffer's reliance on trailing DISPLAY_RAM slack).
#define RV_FB1_STRIDE 32
uint8_t pd_render_fb1[384 * RV_FB1_STRIDE + 64];

static inline uint8_t rv_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xf0) >> 4) | ((v & 0x0f) << 4));
    v = (uint8_t)(((v & 0xcc) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xaa) >> 1) | ((v & 0x55) << 1));
    return v;
}
#endif

void update_texture_cache_soft(void) {
    for (int t = 0; t < 2048; t++) {
		// skip if this tile wasn't modified
		if (tDSPCACHE.CharacterCache[t])
			tDSPCACHE.CharacterCache[t] = false;
		else
			continue;

		uint32_t *tile = (uint32_t*)(vb_state->V810_DISPLAY_RAM.off + ((t & 0x600) << 6) + 0x6000 + (t & 0x1ff) * 16);

		// optimize invisible tiles
		{
			bool tv = ((uint64_t*)tile)[0] | ((uint64_t*)tile)[1];
			tileVisible[t] = tv;
			if (!tv) {
                memset(&tileCache[t].indices, 0, sizeof(tileCache[t].indices));
                memset(&tileCache[t].colmask, 0, sizeof(tileCache[t].colmask));
                memset(&tileCache[t].mask, -1, sizeof(tileCache[t].mask));
#ifdef RV_1BPP
                memset(tileCache1[t].mask, -1, sizeof(tileCache1[t].mask));
                memset(tileCache1[t].idx, 0, sizeof(tileCache1[t].idx));
#endif
                continue;
            }
		}

        for (int i = 0; i < 4; i++) {
            uint32_t column = 0;
            for (int j = 0; j < 4; j++) {
                uint32_t row = tile[j];
                column |= (
                    (row & (0x03 << 4*i)) |
                    ((row & (0x0c << (4*i))) << 14) |
                    ((row & (0x030000 << (4*i))) >> 14) |
                    ((row & (0x0c0000) << 4*i))
                ) >> 4*i << 4*j;
            }
            tileCache[t].indices.u32[i] = column;
            uint32_t tmp;
            uint32_t colmask[3];
            tmp = (column & (~(column & 0xaaaaaaaa) >> 1)) & 0x55555555;
            colmask[0] = tmp | (tmp << 1);
            tmp = (column & (~(column & 0x55555555) << 1)) & 0xaaaaaaaa;
            colmask[1] = tmp | (tmp >> 1);
            tmp = ((column & 0xaaaaaaaa) >> 1) & column;
            colmask[2] = tmp | (tmp << 1);
            for (int k = 0; k < 3; k++) {
                for (int l = 0; l < 4; l++) {
                    const uint32_t cols[4] = {0, 0x55555555, 0xaaaaaaaa, 0xffffffff};
                    uint32_t columns = colmask[k] & cols[l];
                    tileCache[t].colmask.column[i * 2].col[k].shade[l] = columns;
                    tileCache[t].colmask.column[i * 2 + 1].col[k].shade[l] = columns >> 16;
                }
            }
            tileCache[t].mask.u32[i] = ~(colmask[0] | colmask[1] | colmask[2]);
        }
#ifdef RV_1BPP
        // Derive the 1bpp masks from the just-computed 2bpp index columns. Each
        // indices.u16[x] packs 8 pixels as 2-bit colour indices (pixel/row r at
        // bits [2r..2r+1]); collapse to per-index 8-bit row masks.
        for (int x = 0; x < 8; x++) {
            uint16_t w = tileCache[t].indices.u16[x];
            uint8_t m = 0, i1 = 0, i2 = 0, i3 = 0;
            for (int r = 0; r < 8; r++) {
                int v = (w >> (2 * r)) & 3;
                uint8_t bit = (uint8_t)(1 << r);
                if (v == 0)      m  |= bit;
                else if (v == 1) i1 |= bit;
                else if (v == 2) i2 |= bit;
                else             i3 |= bit;
            }
            tileCache1[t].mask[x]   = m;
            tileCache1[t].idx[x][0] = 0;
            tileCache1[t].idx[x][1] = i1;
            tileCache1[t].idx[x][2] = i2;
            tileCache1[t].idx[x][3] = i3;
        }
#endif
    }
}

static uint16_t get_tile_column(int tileid, uint16_t pal, int x, bool yflip) {
    int value =
        (tileCache[tileid].colmask.column[x].col[0].shade[(pal >> 2) & 3]) |
        (tileCache[tileid].colmask.column[x].col[1].shade[(pal >> 4) & 3]) |
        (tileCache[tileid].colmask.column[x].col[2].shade[(pal >> 6) & 3]);
    if (yflip) {
        value = __builtin_bswap16(value);
        value = ((value & 0xf0f0) >> 4) | ((value << 4) & 0xf0f0);
        value = ((value & 0xcccc) >> 2) | ((value << 2) & 0xcccc);
    }
    return value;
}

static uint16_t get_tile_mask(int tileid, int x, bool yflip) {
    int value = tileCache[tileid].mask.u16[x];
    if (yflip) {
        value = __builtin_bswap16(value);
        value = ((value & 0xf0f0) >> 4) | ((value << 4) & 0xf0f0);
        value = ((value & 0xcccc) >> 2) | ((value << 2) & 0xcccc);
    }
    return value;
}

template<bool aligned, bool over> void render_normal_world(uint16_t *fb, WORLD *world, int eye, int drawn_fb) {
    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t base_mx = (s16)(world->mx << 3) >> 3;
    int16_t mp = (s16)(world->mp << 1) >> 1;
    int16_t my = (s16)(world->my << 3) >> 3;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;
    int16_t over_tile = world->over & 0x7ff;

    u16 *tilemap = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000);

    int mx = base_mx + (eye == 0 ? -mp : mp);
    int gx = base_gx + (eye == 0 ? -gp : gp);

    bool over_visible = !over || tileVisible[tilemap[over_tile] & 0x07ff];

    int tsy = my >> 3;
    int mapsy = tsy >> 6;
    tsy &= 63;
    if (!over) {
        mapsy &= scy - 1;
    }

    uint8_t gy_shift = ((gy - my) & 7) * 2;
    uint8_t my_shift = (my & 7) * 2;

    u8 *gplt = vb_state->tVIPREG.GPLT;

    for (int x = 0; likely(x < w); x++) {
        if (unlikely(gx + x < 0)) continue;
        if (unlikely(gx + x >= 384)) break;
        int bpx = (mx + x) & 7;
        int tx = (mx + x) >> 3;
        int mapx = tx >> 6;
        tx &= 63;
        if (!over) mapx &= scx - 1;

        uint16_t *column_out = &fb[(gx + x) * 256 / 8];

        int ty = tsy;
        int mapy = mapsy;
        int current_map = mapid + scx * mapy + mapx;

        uint16_t prev_out = 0;
        uint16_t prev_mask = 0xffff >> (16 - gy_shift);

        for (int y = gy - (my & 7); likely(y < gy + h); y += 8) {
            if (unlikely(y >= 224)) break;
            bool use_over = over && ((mapx & (scx - 1)) != mapx || (mapy & (scy - 1)) != mapy);
            uint16_t tile = tilemap[use_over ? over_tile : (64 * 64) * current_map + 64 * ty + tx];
            if (++ty >= 64) {
                ty = 0;
                if ((++mapy & (scy - 1)) == 0 && !over) mapy = 0;
                current_map = mapid + scx * mapy + mapx;
            }
            if (unlikely(y <= -8)) continue;
            uint16_t tileid = tile & 0x07ff;
            if (!tileVisible[tileid] && (aligned || prev_mask == -1 >> (16 - gy_shift))) continue;
            int palette = tile >> 14;
            int px = tile & 0x2000 ? 7 - bpx : bpx;
            int value = get_tile_column(tileid, gplt[palette], px, (tile & 0x1000) != 0);
            uint16_t mask = get_tile_mask(tileid, px, (tile & 0x1000) != 0);
            uint16_t current_out, current_mask;
            if (aligned) {
                current_out = value;
                current_mask = mask;
                if (mask == 0xffff) continue;
            } else {
                current_out = ((value << gy_shift)) | prev_out;
                current_mask = ((mask << gy_shift)) | prev_mask;
                prev_out = (value) >> (16 - gy_shift);
                prev_mask = (mask) >> (16 - gy_shift);
                if (unlikely(y < gy)) {
                    current_mask |= 0xffff >> ((gy & 7) * 2);
                    current_out &= ~current_mask;
                }
            }
            if (unlikely(y < 0)) continue;
            uint16_t *out_word = &column_out[y >> 3];
            *out_word = (*out_word & current_mask) | current_out;
        }
        if (((gy & 7) + h) >= 8 && ((gy + h) & 7) != 0) {
            uint16_t current_out = prev_out;
            uint16_t current_mask = (-1 << gy_shift) | prev_mask;
            uint16_t *out_word = &column_out[(gy - (my & 7) + h) >> 3];
            *out_word = (*out_word & current_mask) | current_out;
        }
    }
}

template<bool over> void render_affine_world(WORLD *world, int drawn_fb) {
    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    int scx_scy_mask = (scx - 1) | ((scy - 1) << 16);
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t base_mx = (s16)(world->mx << 3) >> 3;
    int16_t mp = (s16)(world->mp << 1) >> 1;
    int16_t my = (s16)(world->my << 3) >> 3;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;
    int16_t over_tile = world->over & 0x7ff;

    u16 *tilemap = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000);

    u16 param_base = world->param;
    s16 *params = (s16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000 + param_base * 2);

    u8 *gplt = vb_state->tVIPREG.GPLT;

    for (int eye = 0; eye < SOFT_EYE_COUNT; eye++) {
        if (!(world->head & (0x8000 >> eye)))
            continue;

        uint16_t *fb = (uint16_t*)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);

        int mx = base_mx + (eye == 0 ? -mp : mp);
        int gx = base_gx + (eye == 0 ? -gp : gp);
        for (int y = 0; likely(y < h); y++) {
            if (unlikely(gy + y < 0)) continue;
            if (unlikely(gy + y >= 224)) break;
            int mx = params[y * 8 + 0] << 6;
            s16 mp = params[y * 8 + 1];
            int my = params[y * 8 + 2] << 6;
            s32 dx = params[y * 8 + 3];
            s32 dy = params[y * 8 + 4];
            mx += (mp >= 0 ? mp * eye : -mp * !eye) * dx;
            my += (mp >= 0 ? mp * eye : -mp * !eye) * dy;

            int shift = (((gy + y) & 3) * 2);

            u8 *out_word = &((uint8_t*)(&fb[gx * 256 / 8]))[((gy + y) >> 2)];
            u8 *end = out_word + w * 256 / 4;
            if (gx < 0) {
                mx += dx * -gx;
                my += dy * -gx;
                out_word += -gx * 256 / 4;
            }
            if (gx + w > 384) {
                end = ((uint8_t*)fb) + 0x6000;
            }

            for (; likely(out_word < end); out_word += 256 / 4) {
                if (true) {
                    // storing xmap and ymap in one int here lets us mask/compare with scx/scy in one go,
                    // which is slightly faster than storing them separately
                    int xmap = mx >> (9 + 9);
                    int ymap = my >> (9 + 9);
                    int xmap_ymap = xmap | (ymap << 16);
                    int xmap_ymap_masked = xmap_ymap & scx_scy_mask;
                    int tx = (mx >> (9 + 3)) & 63;
                    // premultiplied by 64
                    int ty_scaled = (my >> (9 + 3 - 6)) & (63 << 6);
                    // note: not doubled because that doesn't help
                    int bpx = (mx >> 9) & 7;
                    // note: this is doubled because that does help
                    int dbpy = (my >> 8) & (7 << 1);
                    int tile_pos;
                    if (over && unlikely(xmap_ymap != xmap_ymap_masked)) {
                        tile_pos = over_tile;
                    } else {
                        int this_map = mapid + (xmap_ymap_masked >> 16) * scx + (xmap_ymap_masked & 0xffff);
                        tile_pos = this_map * 4096 + ty_scaled + tx;
                    }
                    u16 tile = tilemap[tile_pos];
                    u16 tileid = tile & 0x07ff;
                    int palette = tile >> 14;
                    int px = tile & 0x2000 ? 7 - bpx : bpx;
                    int dpy = tile & 0x1000 ? (7 << 1) - dbpy : dbpy;
                    uint16_t tilecolumn = tileCache[tileid].indices.u16[px];
                    int pxindex = (tilecolumn >> dpy) & 3;
                    if (pxindex) {
                        int pxvalue = (gplt[palette] >> (pxindex * 2)) & 3;
                        *out_word = (*out_word & ~(3 << shift)) | (pxvalue << shift);
                    }
                }
                mx += dx;
                my += dy;
            }
        }
    }
}

#ifdef RV_1BPP
// 1bpp equivalents of get_tile_column / get_tile_mask. A pixel is "bright"
// (Playdate white) when its palette-mapped shade is >= 2 (the threshold used by
// the old 2bpp->1bpp blit). The opacity mask is palette-independent.
static inline uint8_t get_tile_col1(int tileid, uint16_t pal, int x, bool yflip) {
    uint8_t v = 0;
    if (((pal >> 2) & 3) >= 2) v |= tileCache1[tileid].idx[x][1];
    if (((pal >> 4) & 3) >= 2) v |= tileCache1[tileid].idx[x][2];
    if (((pal >> 6) & 3) >= 2) v |= tileCache1[tileid].idx[x][3];
    return yflip ? rv_rev8(v) : v;
}

static inline uint8_t get_tile_mask1(int tileid, int x, bool yflip) {
    uint8_t v = tileCache1[tileid].mask[x];
    return yflip ? rv_rev8(v) : v;
}

// 1bpp port of render_normal_world. Same structure, but 8-bit columns: every
// 2-bit shift constant from the 2bpp version is halved, the framebuffer stride
// is 32 bytes/column, and bright/opacity come from the 1bpp tile cache.
template<bool aligned, bool over>
static void render_normal_world_1bpp(uint8_t *fb, WORLD *world, int eye) {
    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t base_mx = (s16)(world->mx << 3) >> 3;
    int16_t mp = (s16)(world->mp << 1) >> 1;
    int16_t my = (s16)(world->my << 3) >> 3;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;
    int16_t over_tile = world->over & 0x7ff;

    u16 *tilemap = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000);

    int mx = base_mx + (eye == 0 ? -mp : mp);
    int gx = base_gx + (eye == 0 ? -gp : gp);

    int tsy = my >> 3;
    int mapsy = tsy >> 6;
    tsy &= 63;
    if (!over) mapsy &= scy - 1;

    uint8_t gy_shift = (gy - my) & 7;

    u8 *gplt = vb_state->tVIPREG.GPLT;

    for (int x = 0; likely(x < w); x++) {
        if (unlikely(gx + x < 0)) continue;
        if (unlikely(gx + x >= 384)) break;
        int bpx = (mx + x) & 7;
        int tx = (mx + x) >> 3;
        int mapx = tx >> 6;
        tx &= 63;
        if (!over) mapx &= scx - 1;

        uint8_t *column_out = &fb[(gx + x) * RV_FB1_STRIDE];

        int ty = tsy;
        int mapy = mapsy;
        int current_map = mapid + scx * mapy + mapx;

        uint8_t prev_out = 0;
        uint8_t prev_mask = (uint8_t)(0xff >> (8 - gy_shift));

        for (int y = gy - (my & 7); likely(y < gy + h); y += 8) {
            if (unlikely(y >= 224)) break;
            bool use_over = over && ((mapx & (scx - 1)) != mapx || (mapy & (scy - 1)) != mapy);
            uint16_t tile = tilemap[use_over ? over_tile : (64 * 64) * current_map + 64 * ty + tx];
            if (++ty >= 64) {
                ty = 0;
                if ((++mapy & (scy - 1)) == 0 && !over) mapy = 0;
                current_map = mapid + scx * mapy + mapx;
            }
            if (unlikely(y <= -8)) continue;
            uint16_t tileid = tile & 0x07ff;
            if (!tileVisible[tileid] && (aligned || prev_mask == (uint8_t)(0xff >> (8 - gy_shift)))) continue;
            int palette = tile >> 14;
            int px = tile & 0x2000 ? 7 - bpx : bpx;
            uint8_t value = get_tile_col1(tileid, gplt[palette], px, (tile & 0x1000) != 0);
            uint8_t mask = get_tile_mask1(tileid, px, (tile & 0x1000) != 0);
            uint8_t current_out, current_mask;
            if (aligned) {
                current_out = value;
                current_mask = mask;
                if (mask == 0xff) continue;
            } else {
                current_out = (uint8_t)((value << gy_shift)) | prev_out;
                current_mask = (uint8_t)((mask << gy_shift)) | prev_mask;
                prev_out = (uint8_t)(value >> (8 - gy_shift));
                prev_mask = (uint8_t)(mask >> (8 - gy_shift));
                if (unlikely(y < gy)) {
                    current_mask |= (uint8_t)(0xff >> (gy & 7));
                    current_out &= ~current_mask;
                }
            }
            if (unlikely(y < 0)) continue;
            uint8_t *out_word = &column_out[y >> 3];
            *out_word = (*out_word & current_mask) | current_out;
        }
        if (((gy & 7) + h) >= 8 && ((gy + h) & 7) != 0) {
            int tail = (gy - (my & 7) + h) >> 3;
            if ((unsigned)tail < RV_FB1_STRIDE) {
                uint8_t current_out = prev_out;
                uint8_t current_mask = (uint8_t)(0xff << gy_shift) | prev_mask;
                uint8_t *out_word = &column_out[tail];
                *out_word = (*out_word & current_mask) | current_out;
            }
        }
    }
}

// 1bpp port of render_affine_world (left eye only). Like the 2bpp version it
// samples tileCache[].indices per pixel and thresholds the palette-mapped shade
// (>= 2 = white); only the framebuffer write differs (1 bit vs 2).
template<bool over>
static void render_affine_world_1bpp(WORLD *world) {
    const int eye = 0;
    if (!(world->head & (0x8000 >> eye))) return;  // left eye not enabled

    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    int scx_scy_mask = (scx - 1) | ((scy - 1) << 16);
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;
    int16_t over_tile = world->over & 0x7ff;

    u16 *tilemap = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000);
    u16 param_base = world->param;
    s16 *params = (s16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000 + param_base * 2);
    u8 *gplt = vb_state->tVIPREG.GPLT;

    int gx = base_gx - gp;  // eye 0
    uint8_t *fb = pd_render_fb1;

    // Per-tile cache: the affine walk steps sub-pixel, so consecutive pixels
    // usually fall in the same tile. We recompute the (expensive) tilemap read,
    // tile flags and palette→bright table only when tile_pos changes, leaving
    // the per-pixel loop to do just coordinate math + one tile-data read + the
    // write. bright[c] = does colour index c map (via this tile's palette) to a
    // shade >= the white threshold; bright[0] stays 0 (index 0 is transparent).
    int prev_tile_pos = -1;
    int c_tileid = 0;
    bool c_hflip = false, c_vflip = false;
    uint8_t bright[4] = {0, 0, 0, 0};

    for (int y = 0; likely(y < h); y++) {
        if (unlikely(gy + y < 0)) continue;
        if (unlikely(gy + y >= 224)) break;
        int mx = params[y * 8 + 0] << 6;
        s16 mp = params[y * 8 + 1];
        int my = params[y * 8 + 2] << 6;
        s32 dx = params[y * 8 + 3];
        s32 dy = params[y * 8 + 4];
        // eye 0: mp*eye == 0, -mp*!eye == -mp
        mx += (mp >= 0 ? 0 : -mp) * dx;
        my += (mp >= 0 ? 0 : -mp) * dy;

        uint8_t bit = (uint8_t)(1 << ((gy + y) & 7));

        uint8_t *out_word = &fb[gx * RV_FB1_STRIDE + ((gy + y) >> 3)];
        uint8_t *end = out_word + w * RV_FB1_STRIDE;
        if (gx < 0) {
            mx += dx * -gx;
            my += dy * -gx;
            out_word += -gx * RV_FB1_STRIDE;
        }
        if (gx + w > 384) {
            end = fb + 384 * RV_FB1_STRIDE;
        }

        // Span fast path for the common scale-only case (no rotation term):
        // when dy == 0 the source row is constant across the scanline, so the
        // tilemap read, flip flags, palette table AND the texel index are all
        // invariant for whole runs of output pixels. Walk the line as runs —
        // run = how many output pixels sample the same source pixel (ceil of
        // remaining sub-pixel distance / dx) — doing only the bit write per
        // pixel inside a run, and re-fetching tile data only at 8-px tile
        // boundaries. Magnified rows (dx < 0x200) get runs of 2+; minified
        // rows degrade gracefully to runs of 1. Fixed-point recap: 1 source
        // px = 0x200 units, 1 tile = 0x1000, 1 map (512 px) = 1<<18.
        // dx <= 0 (mirrored/degenerate) stays on the generic path, and so do
        // minified/1:1 rows (dx >= 0x200): their runs are all length 1, so the
        // span bookkeeping (one SDIV per run) costs more than it saves —
        // measured +3 ms vip on Mario's Tennis's zoomed-out title court.
        if (dy == 0 && dx > 0 && dx < 0x200) {
            int ymap = my >> (9 + 9);
            int ty_scaled = (my >> (9 + 3 - 6)) & (63 << 6);
            int dbpy = (my >> 8) & (7 << 1);
            while (likely(out_word < end)) {
                // Per-tile work (identical math to the generic loop below).
                int xmap = mx >> (9 + 9);
                int xmap_ymap = xmap | (ymap << 16);
                int xmap_ymap_masked = xmap_ymap & scx_scy_mask;
                int tx = (mx >> (9 + 3)) & 63;
                int tile_pos;
                if (over && unlikely(xmap_ymap != xmap_ymap_masked)) {
                    tile_pos = over_tile;
                } else {
                    int this_map = mapid + (xmap_ymap_masked >> 16) * scx + (xmap_ymap_masked & 0xffff);
                    tile_pos = this_map * 4096 + ty_scaled + tx;
                }
                if (tile_pos != prev_tile_pos) {
                    prev_tile_pos = tile_pos;
                    u16 tile = tilemap[tile_pos];
                    c_tileid = tile & 0x07ff;
                    c_hflip = (tile & 0x2000) != 0;
                    c_vflip = (tile & 0x1000) != 0;
                    int pal = gplt[tile >> 14];
                    bright[1] = (uint8_t)(((pal >> 2) & 3) >= 2);
                    bright[2] = (uint8_t)(((pal >> 4) & 3) >= 2);
                    bright[3] = (uint8_t)(((pal >> 6) & 3) >= 2);
                }
                int dpy = c_vflip ? (7 << 1) - dbpy : dbpy;
                int tile_end_mx = (mx & ~0xfff) + 0x1000;
                do {
                    int bpx = (mx >> 9) & 7;
                    int px = c_hflip ? 7 - bpx : bpx;
                    int pxindex = (tileCache[c_tileid].indices.u16[px] >> dpy) & 3;
                    // Output pixels sampling this same source pixel.
                    int run = (0x200 - (mx & 0x1ff) + dx - 1) / dx;
                    // Clamp to remaining output columns (ceil: out_word
                    // carries the row-byte offset, end may not).
                    int max_run = (int)((end - out_word + RV_FB1_STRIDE - 1) / RV_FB1_STRIDE);
                    if (run > max_run) run = max_run;
                    if (pxindex == 0) {
                        out_word += (size_t)run * RV_FB1_STRIDE;   // transparent
                    } else if (bright[pxindex]) {
                        for (int r = run; r > 0; r--) { *out_word |= bit; out_word += RV_FB1_STRIDE; }
                    } else {
                        for (int r = run; r > 0; r--) { *out_word &= (uint8_t)~bit; out_word += RV_FB1_STRIDE; }
                    }
                    mx += dx * run;
                } while (likely(out_word < end) && mx < tile_end_mx);
            }
            continue;
        }

        for (; likely(out_word < end); out_word += RV_FB1_STRIDE) {
            int xmap = mx >> (9 + 9);
            int ymap = my >> (9 + 9);
            int xmap_ymap = xmap | (ymap << 16);
            int xmap_ymap_masked = xmap_ymap & scx_scy_mask;
            int tx = (mx >> (9 + 3)) & 63;
            int ty_scaled = (my >> (9 + 3 - 6)) & (63 << 6);
            int bpx = (mx >> 9) & 7;
            int dbpy = (my >> 8) & (7 << 1);
            int tile_pos;
            if (over && unlikely(xmap_ymap != xmap_ymap_masked)) {
                tile_pos = over_tile;
            } else {
                int this_map = mapid + (xmap_ymap_masked >> 16) * scx + (xmap_ymap_masked & 0xffff);
                tile_pos = this_map * 4096 + ty_scaled + tx;
            }
            if (tile_pos != prev_tile_pos) {
                prev_tile_pos = tile_pos;
                u16 tile = tilemap[tile_pos];
                c_tileid = tile & 0x07ff;
                c_hflip = (tile & 0x2000) != 0;
                c_vflip = (tile & 0x1000) != 0;
                int pal = gplt[tile >> 14];
                bright[1] = (uint8_t)(((pal >> 2) & 3) >= 2);
                bright[2] = (uint8_t)(((pal >> 4) & 3) >= 2);
                bright[3] = (uint8_t)(((pal >> 6) & 3) >= 2);
            }
            int px  = c_hflip ? 7 - bpx : bpx;
            int dpy = c_vflip ? (7 << 1) - dbpy : dbpy;
            int pxindex = (tileCache[c_tileid].indices.u16[px] >> dpy) & 3;
            if (pxindex) {
                if (bright[pxindex]) *out_word |= bit;
                else                 *out_word &= (uint8_t)~bit;
            }
            mx += dx;
            my += dy;
        }
    }
}

// 1bpp port of h-bias (bgm==1): a normal, *unscaled* background where every
// scanline gets its own horizontal shift `p` read from the param table
// (u = mx + p, v = my + y). Because the shift varies per line we can't reuse
// the column-major tile walk of render_normal_world_1bpp — we sample per pixel
// like the affine path, just with an integer 1:1 step instead of a fractional
// one. Left eye only (eye_offset is therefore 0, so we always read params[y*2]).
template<bool over>
static void render_hbias_world_1bpp(WORLD *world) {
    const int eye = 0;
    if (!(world->head & (0x8000 >> eye))) return;  // left eye not enabled

    uint8_t mapid = world->head & 0xf;
    uint8_t scx_pow = ((world->head >> 10) & 3);
    uint8_t scy_pow = ((world->head >> 8) & 3);
    uint8_t scx = 1 << scx_pow;
    uint8_t scy = 1 << scy_pow;
    int16_t base_gx = (s16)(world->gx << 6) >> 6;
    int16_t gp = (s16)(world->gp << 6) >> 6;
    int16_t gy = world->gy;
    int16_t base_mx = (s16)(world->mx << 3) >> 3;
    int16_t mp = (s16)(world->mp << 1) >> 1;
    int16_t my = (s16)(world->my << 3) >> 3;
    int16_t w = world->w + 1;
    int16_t h = world->h + 1;
    int16_t over_tile = world->over & 0x7ff;

    u16 *tilemap = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000);
    u16 param_base = world->param;
    s16 *params = (s16 *)(vb_state->V810_DISPLAY_RAM.off + 0x20000 + param_base * 2);
    u8 *gplt = vb_state->tVIPREG.GPLT;

    int mx = base_mx - mp;  // eye 0
    int gx = base_gx - gp;
    uint8_t *fb = pd_render_fb1;

    // Per-tile cache, same idea as the affine path: recompute the tilemap read,
    // flip flags and palette→bright table only when the source tile changes
    // (every 8 source pixels), leaving the inner loop to do coordinate math, one
    // tile-data read and the write. bright[c] = does index c map (via this
    // tile's palette) to a shade >= the white threshold; index 0 is transparent.
    int prev_tile_pos = -1;
    int c_tileid = 0;
    bool c_hflip = false, c_vflip = false;
    uint8_t bright[4] = {0, 0, 0, 0};

    for (int y = 0; likely(y < h); y++) {
        int sy = gy + y;
        if (unlikely(sy < 0)) continue;
        if (unlikely(sy >= 224)) break;

        // 13-bit signed horizontal offset for this line (left eye = params[y*2]).
        s16 p = (s16)(params[y * 2] << 3) >> 3;
        int u0 = mx + p;
        int v = my + y;

        int vty = v >> 3;
        int vmapy = vty >> 6;
        vty &= 63;
        if (!over) vmapy &= scy - 1;
        int dbpy = (v & 7) << 1;

        uint8_t bit = (uint8_t)(1 << (sy & 7));
        int row = sy >> 3;

        for (int x = 0; likely(x < w); x++) {
            int cx = gx + x;
            if (unlikely(cx < 0)) continue;
            if (unlikely(cx >= 384)) break;

            int u = u0 + x;
            int tx = u >> 3;
            int umapx = tx >> 6;
            tx &= 63;
            if (!over) umapx &= scx - 1;
            int bpx = u & 7;

            int tile_pos;
            if (over && unlikely((umapx & (scx - 1)) != umapx || (vmapy & (scy - 1)) != vmapy)) {
                tile_pos = over_tile;
            } else {
                int this_map = mapid + scx * vmapy + umapx;
                tile_pos = this_map * 4096 + 64 * vty + tx;
            }
            if (tile_pos != prev_tile_pos) {
                prev_tile_pos = tile_pos;
                u16 tile = tilemap[tile_pos];
                c_tileid = tile & 0x07ff;
                c_hflip = (tile & 0x2000) != 0;
                c_vflip = (tile & 0x1000) != 0;
                int pal = gplt[tile >> 14];
                bright[1] = (uint8_t)(((pal >> 2) & 3) >= 2);
                bright[2] = (uint8_t)(((pal >> 4) & 3) >= 2);
                bright[3] = (uint8_t)(((pal >> 6) & 3) >= 2);
            }
            int px  = c_hflip ? 7 - bpx : bpx;
            int dpy = c_vflip ? (7 << 1) - dbpy : dbpy;
            int pxindex = (tileCache[c_tileid].indices.u16[px] >> dpy) & 3;
            if (pxindex) {
                uint8_t *out_word = &fb[cx * RV_FB1_STRIDE + row];
                if (bright[pxindex]) *out_word |= bit;
                else                 *out_word &= (uint8_t)~bit;
            }
        }
    }
}

// 1bpp render path: composites the left eye of all enabled normal/object/affine/
// h-bias worlds into pd_render_fb1.
void video_soft_render_1bpp(void) {
    memset(pd_render_fb1, 0, 384 * RV_FB1_STRIDE);

    uint8_t object_group_id = 3;
    WORLD *worlds = (WORLD *)(vb_state->V810_DISPLAY_RAM.off + 0x3d800);
    for (int wrld = 31; wrld >= 0; wrld--) {
        if (worlds[wrld].end)
            break;
        if (worlds[wrld].on == 0)
            continue;

        if (worlds[wrld].bgm == 0) {
            // normal world, left eye only (on bit 1 = 2>>0)
            if (!(worlds[wrld].on & 2))
                continue;
            int16_t gy = worlds[wrld].gy;
            int16_t my = (s16)(worlds[wrld].my << 3) >> 3;
            int16_t h = worlds[wrld].h + 1;
            bool over = worlds[wrld].is_over;
            if ((gy & 7) || (my & 7) || (h & 7)) {
                if (over)
                    render_normal_world_1bpp<false, true>(pd_render_fb1, &worlds[wrld], 0);
                else
                    render_normal_world_1bpp<false, false>(pd_render_fb1, &worlds[wrld], 0);
            } else {
                if (over)
                    render_normal_world_1bpp<true, true>(pd_render_fb1, &worlds[wrld], 0);
                else
                    render_normal_world_1bpp<true, false>(pd_render_fb1, &worlds[wrld], 0);
            }
        } else if (worlds[wrld].bgm == 3) {
            // object world
            int start_index = object_group_id == 0 ? 1023 : (vb_state->tVIPREG.SPT[object_group_id - 1]) & 1023;
            int end_index = vb_state->tVIPREG.SPT[object_group_id] & 1023;
            for (int i = end_index; i != start_index; i = (i - 1) & 1023) {
                u16 *obj_ptr = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x0003E000 + 8 * i);

                u16 cw3 = obj_ptr[3];
                u16 tileid = cw3 & 0x07ff;
                if (!tileVisible[tileid]) continue;
                if (!(obj_ptr[1] & 0x8000)) continue;  // left eye only

                u16 base_x = obj_ptr[0];
                u16 cw1 = obj_ptr[1];
                s16 y = *(u8*)&obj_ptr[2];
                if (y > 224) y = (s8)y;

                short palette = (cw3 >> 14);
                s16 jp = (s16)(cw1 << 6) >> 6;

                s16 x = base_x - jp;   // left eye

                int sh = y & 7;
                for (int bpx = 0; bpx < 8; bpx++) {
                    if (x + bpx < 0) continue;
                    if (x + bpx >= 384) break;
                    int px = cw3 & 0x2000 ? 7 - bpx : bpx;
                    uint8_t value = get_tile_col1(tileid, vb_state->tVIPREG.JPLT[palette], px, (cw3 & 0x1000) != 0);
                    uint8_t mask = get_tile_mask1(tileid, px, (cw3 & 0x1000) != 0);
                    if (mask == 0xff) continue;

                    uint8_t *out_word = &pd_render_fb1[(y >> 3) + (x + bpx) * RV_FB1_STRIDE];
                    if (y >= 0) {
                        *out_word = (*out_word & (uint8_t)((mask << sh) | (uint8_t)(0xff >> (8 - sh)))) | (uint8_t)(value << sh);
                    }
                    if (sh && y < 224 - 8) {
                        out_word++;
                        *out_word = (*out_word & (uint8_t)((mask >> (8 - sh)) | (uint8_t)(0xff << sh))) | (uint8_t)(value >> (8 - sh));
                    }
                }
            }
            object_group_id = (object_group_id - 1) & 3;
        } else if (worlds[wrld].bgm == 2) {
            // affine world
            if (worlds[wrld].is_over)
                render_affine_world_1bpp<true>(&worlds[wrld]);
            else
                render_affine_world_1bpp<false>(&worlds[wrld]);
        } else if (worlds[wrld].bgm == 1) {
            // h-bias world
            if (worlds[wrld].is_over)
                render_hbias_world_1bpp<true>(&worlds[wrld]);
            else
                render_hbias_world_1bpp<false>(&worlds[wrld]);
        }
    }
}
#endif

void video_soft_render(int drawn_fb) {
    tDSPCACHE.DDSPDataState[drawn_fb] = CPU_WROTE;
    #ifdef __3DS__
    uint32_t fb_size;
    uint32_t *out_fb = (uint32_t*)C3D_Tex2DGetImagePtr(&screenTexSoft[drawn_fb], 0, &fb_size);
    memset(out_fb, 0, fb_size);
    #endif
	    uint8_t object_group_id = 3;
    for (int eye = 0; eye < SOFT_EYE_COUNT; eye++) {
        uint16_t *fb = (uint16_t*)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);
        memset(fb, 0, 0x6000);
    }
    WORLD *worlds = (WORLD *)(vb_state->V810_DISPLAY_RAM.off + 0x3d800);
    for (int wrld = 31; wrld >= 0; wrld--) {
        if (worlds[wrld].end)
            break;
        if (worlds[wrld].on == 0)
            continue;

        // set softbuf modified area for background worlds.
        // SoftBufWrote drives the 3DS GPU partial-upload path; on Playdate we
        // blit the whole frame, so this per-column bookkeeping is pure
        // overhead — skip it there.
#if !defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)
        if (worlds[wrld].bgm != 3) {
            int16_t base_gx = (s16)(worlds[wrld].gx << 6) >> 6;
            int16_t gp = (s16)(worlds[wrld].gp << 6) >> 6;
            int16_t gy = worlds[wrld].gy;
            int16_t w = worlds[wrld].w + 1;
            int16_t h = worlds[wrld].h + 1;
            int left_gx = base_gx - gp;
            int right_gx = base_gx + gp;
            int min_gx, max_gx;
            if (worlds[wrld].on == 0b11) {
                min_gx = left_gx < right_gx ? left_gx : right_gx;
                max_gx = left_gx > right_gx ? left_gx : right_gx;
            } else {
                min_gx = max_gx = worlds[wrld].on & 2 ? left_gx : right_gx;
            }
            for (int x = min_gx & ~7; x < max_gx + abs(gp) + w && x < 384; x += 8) {
                if (x < 0) continue;
                SOFTBOUND *column = &tDSPCACHE.SoftBufWrote[drawn_fb][x / 8];
                int min = gy / 8;
                if (min < 0) min = 0;
                if (column->min > min) column->min = min;
                int max = (gy + h - 1) / 8;
                if (max < 0) max = 0;
                if (max > 31) max = 31;
                if (column->max < max) column->max = max;
            }
        }
#endif

        if (worlds[wrld].bgm == 0) {
            // normal world
            for (int eye = 0; eye < SOFT_EYE_COUNT; eye++) {
                if (!(worlds[wrld].on & (2 >> eye)))
                    continue;
                uint16_t *fb = (uint16_t*)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);
                int16_t gy = worlds[wrld].gy;
                int16_t my = (s16)(worlds[wrld].my << 3) >> 3;
                int16_t h = worlds[wrld].h + 1;
                bool over = worlds[wrld].is_over;
                if ((gy & 7) || (my & 7) || (h & 7)) {
                    if (over)
                        render_normal_world<false, true>(fb, &worlds[wrld], eye, drawn_fb);
                    else
                        render_normal_world<false, false>(fb, &worlds[wrld], eye, drawn_fb);
                } else {
                    if (over)
                        render_normal_world<true, true>(fb, &worlds[wrld], eye, drawn_fb);
                    else
                        render_normal_world<true, false>(fb, &worlds[wrld], eye, drawn_fb);
                }
            }
        } else if (worlds[wrld].bgm == 1) {
            // h-bias world
            // TODO
        } else if (worlds[wrld].bgm == 2) {
            // affine world
            bool over = worlds[wrld].is_over;
            if (over) {
                render_affine_world<true>(&worlds[wrld], drawn_fb);
            } else {
                render_affine_world<false>(&worlds[wrld], drawn_fb);
            }
        } else {
            // object world
            int start_index = object_group_id == 0 ? 1023 : (vb_state->tVIPREG.SPT[object_group_id - 1]) & 1023;
            int end_index = vb_state->tVIPREG.SPT[object_group_id] & 1023;
            for (int i = end_index; i != start_index; i = (i - 1) & 1023) {
                u16 *obj_ptr = (u16 *)(vb_state->V810_DISPLAY_RAM.off + 0x0003E000 + 8 * i);

                u16 cw3 = obj_ptr[3];
                u16 tileid = cw3 & 0x07ff;
                if (!tileVisible[tileid]) continue;

                u16 base_x = obj_ptr[0];
                u16 cw1 = obj_ptr[1];
                s16 y = *(u8*)&obj_ptr[2];
                if (y > 224) y = (s8)y;

                short palette = (cw3 >> 14);

                s16 jp = (s16)(cw1 << 6) >> 6;

                // SoftBufWrote bounds: 3DS GPU-upload only; skip on Playdate.
#if !defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)
                for (int x = (base_x - abs(jp)) & ~7; x < base_x + abs(jp) && x < 384; x += 8) {
                    if (x < 0) continue;
                    SOFTBOUND *column = &tDSPCACHE.SoftBufWrote[drawn_fb][x / 8];
                    int min = y / 8;
                    if (min < 0) min = 0;
                    if (column->min > min) column->min = min;
                    int max = (y + 7) / 8;
                    if (max < 0) max = 0;
                    if (max > 31) max = 31;
                    if (column->max < max) column->max = max;
                }
#endif

                for (int eye = 0; eye < SOFT_EYE_COUNT; eye++) {
                    if (!(cw1 & (0x8000 >> eye)))
                        continue;

                    uint16_t *fb = (uint16_t*)(vb_state->V810_DISPLAY_RAM.off + 0x10000 * eye + 0x8000 * drawn_fb);

                    s16 x = base_x;
                    if (eye == 0)
                        x -= jp;
                    else
                        x += jp;
                    
                    for (int bpx = 0; bpx < 8; bpx++) {
                        if (x + bpx < 0) continue;
                        if (x + bpx >= 384) break;
                        int px = cw3 & 0x2000 ? 7 - bpx : bpx;
                        int value = get_tile_column(tileid, vb_state->tVIPREG.JPLT[palette], px, (cw3 & 0x1000) != 0);
                        uint16_t mask = get_tile_mask(tileid, px, (cw3 & 0x1000) != 0);
                        if (mask == 0xffff) continue;
                        
                        uint16_t *out_word = &fb[((y) >> 3) + ((x + bpx) * 256 / 8)];
                        if (y >= 0) {
                            *out_word = (*out_word & ((mask << ((y & 7) * 2)) | ((u16)-1 >> (16 - (y & 7) * 2)))) | (value << ((y & 7) * 2));
                        }

                        if ((y & 7) && y < 224-8) {
                            out_word++;
                            *out_word = (*out_word & ((mask >> (16 - (y & 7) * 2) | (-1 << ((y & 7) * 2))))) | (value >> (16 - (y & 7) * 2));
                        }
                    }
                }
            }
            object_group_id = (object_group_id - 1) & 3;
        }
    }
}