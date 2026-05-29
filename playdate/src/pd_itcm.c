#include <stdint.h>
#include <string.h>

#include "pd_itcm.h"
#include "vb_types.h"
#include "v810_cpu.h"
#include "v810_mem.h"

// Default routing: the plain SDRAM mem_* accessors. On device these get
// repointed into the DTCM copy by rv_itcm_relocate; everywhere else they stay
// as-is, so behaviour is identical with relocation disabled.
rv_read_fn  g_rv_rbyte;
rv_read_fn  g_rv_rhword;
rv_read_fn  g_rv_rword;
rv_write_fn g_rv_wbyte;
rv_write_fn g_rv_whword;
rv_write_fn g_rv_wword;

// Fallback pointers used by the relocated fast-paths for the rare regions
// (VIP/HW/sound/SRAM). Called indirectly so the relocated code reaches them
// regardless of how far it was moved (no PC-relative range limit).
static rv_read_fn  s_fb_rbyte;
static rv_read_fn  s_fb_rhword;
static rv_read_fn  s_fb_rword;
static rv_write_fn s_fb_wbyte;
static rv_write_fn s_fb_whword;
static rv_write_fn s_fb_wword;

void rv_itcm_init(void) {
    g_rv_rbyte  = mem_rbyte;   s_fb_rbyte  = mem_rbyte;
    g_rv_rhword = mem_rhword;  s_fb_rhword = mem_rhword;
    g_rv_rword  = mem_rword;   s_fb_rword  = mem_rword;
    g_rv_wbyte  = mem_wbyte;   s_fb_wbyte  = mem_wbyte;
    g_rv_whword = mem_whword;  s_fb_whword = mem_whword;
    g_rv_wword  = mem_wword;   s_fb_wword  = mem_wword;
}

#if defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)

#define ROM_MASK (0x07000000u | (MAX_ROM_SIZE - 1))

// Hot fast-path accessors, relocated into DTCM. WRAM (and ROM for reads) are
// handled inline; every other region defers to the full mem_* via an indirect
// (pointer) call so it works from any relocated address.
#define ITCM __attribute__((section(".itcm.rv"), noinline))

ITCM static uint64_t rv_rbyte_impl(WORD addr) {
    WORD region = addr & 0x07000000u;
    if (region == 0x05000000u)
        return (uint64_t)(WORD)((SBYTE *)(vb_state->V810_VB_RAM.off + (addr & 0x0500ffffu)))[0];
    if (region == 0x07000000u)
        return (uint64_t)(WORD)((SBYTE *)(V810_ROM1.off + (addr & ROM_MASK)))[0];
    return s_fb_rbyte(addr);
}
ITCM static uint64_t rv_rhword_impl(WORD addr) {
    WORD region = addr & 0x07000000u;
    if (region == 0x05000000u)
        return (uint64_t)(WORD)((SHWORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffeu)))[0];
    if (region == 0x07000000u)
        return (uint64_t)(WORD)((SHWORD *)(V810_ROM1.off + (addr & ROM_MASK & ~1u)))[0];
    return s_fb_rhword(addr);
}
ITCM static uint64_t rv_rword_impl(WORD addr) {
    WORD region = addr & 0x07000000u;
    if (region == 0x05000000u)
        return (uint64_t)((WORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffcu)))[0];
    if (region == 0x07000000u)
        return (uint64_t)((WORD *)(V810_ROM1.off + (addr & ROM_MASK & ~3u)))[0];
    return s_fb_rword(addr);
}
ITCM static WORD rv_wbyte_impl(WORD addr, WORD data) {
    if ((addr & 0x07000000u) == 0x05000000u) {
        ((BYTE *)(vb_state->V810_VB_RAM.off + (addr & 0x0500ffffu)))[0] = (BYTE)data;
        return 0;
    }
    return s_fb_wbyte(addr, data);
}
ITCM static WORD rv_whword_impl(WORD addr, WORD data) {
    if ((addr & 0x07000000u) == 0x05000000u) {
        ((HWORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffeu)))[0] = (HWORD)data;
        return 0;
    }
    return s_fb_whword(addr, data);
}
ITCM static WORD rv_wword_impl(WORD addr, WORD data) {
    if ((addr & 0x07000000u) == 0x05000000u) {
        ((WORD *)(vb_state->V810_VB_RAM.off + (addr & 0x0500fffcu)))[0] = data;
        return 0;
    }
    return s_fb_wword(addr, data);
}

extern char __itcm_rv_start[];
extern char __itcm_rv_end[];

static inline void dsb(void) { __asm__ volatile("dsb 0xf" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb 0xf" ::: "memory"); }

uint32_t rv_itcm_size(void) {
    return (uint32_t)(__itcm_rv_end - __itcm_rv_start);
}

int rv_itcm_relocate(void *buf, uint32_t buf_size) {
    uint32_t sz = rv_itcm_size();
    if (sz == 0 || sz > buf_size) return 0;

    memcpy(buf, __itcm_rv_start, sz);
    dsb();
    isb();

    uintptr_t start = (uintptr_t)__itcm_rv_start;
    uintptr_t base  = (uintptr_t)buf;
    #define REMAP(fn) ((base + (((uintptr_t)(fn) & ~1u) - start)) | 1u)
    g_rv_rbyte  = (rv_read_fn) REMAP(rv_rbyte_impl);
    g_rv_rhword = (rv_read_fn) REMAP(rv_rhword_impl);
    g_rv_rword  = (rv_read_fn) REMAP(rv_rword_impl);
    g_rv_wbyte  = (rv_write_fn)REMAP(rv_wbyte_impl);
    g_rv_whword = (rv_write_fn)REMAP(rv_whword_impl);
    g_rv_wword  = (rv_write_fn)REMAP(rv_wword_impl);
    #undef REMAP
    return 1;
}

#else // simulator / non-device

uint32_t rv_itcm_size(void) { return 0; }
int rv_itcm_relocate(void *buf, uint32_t buf_size) { (void)buf; (void)buf_size; return 0; }

#endif
