#include "pd_jit_run.h"
#include "pd_jit.h"

#include "vb_types.h"
#include "v810_cpu.h"
#include "v810_mem.h"

// Single global dispatcher state. The interpreter reaches it only through the
// rv_jit_* functions below.
static jit_state    g_jit;
static int          g_jit_ok = 0;
static PlaydateAPI *g_pd = 0;

// Diagnostics: blocks executed this window, translations (extern counter), and
// a frame counter to log every so often.
extern uint32_t g_ra_xlate_count;
static uint32_t g_exec = 0;
static uint32_t g_frames = 0;
static uint32_t g_xlate_last = 0;

// ROM address -> ROM buffer halfword pointer, matching itrp_fetch()'s masking.
#define ROM_MASK ((0x07000000u | (MAX_ROM_SIZE - 1)) & ~1u)

void rv_jit_init(PlaydateAPI *pd) {
    // 256 KB of executable code cache, 16K block-map slots. Both live in the
    // ~8 MB heap (SDRAM), which the RWX probe confirmed is executable.
    g_pd = pd;
    g_jit_ok = pd_jit_init(&g_jit, pd, 256 * 1024, 16384) ? 1 : 0;
    if (g_jit_ok)
        pd->system->logToConsole("rv_jit: enabled (256KB code, 16384 blocks)");
    else
        pd->system->logToConsole("rv_jit: init failed; dispatcher disabled");
}

void rv_jit_frame_flush(void) {
    if (!g_jit_ok) return;
    pd_jit_flush(&g_jit);
    // Every 60 frames, report block executions and translations since the last
    // report (diagnostic for where dynarec time goes: steady exec vs xlate churn).
    if (++g_frames >= 60) {
        uint32_t xl = g_ra_xlate_count - g_xlate_last;
        g_pd->system->logToConsole("rv_jit: exec=%lu/60f xlate=%lu/60f blocks=%lu",
                                   (unsigned long)g_exec, (unsigned long)xl,
                                   (unsigned long)g_jit.block_count);
        g_exec = 0;
        g_xlate_last = g_ra_xlate_count;
        g_frames = 0;
    }
}

uint32_t rv_jit_dispatch(uint32_t pc, uint32_t *cyc, uint8_t *last_op) {
    if (!g_jit_ok) return RV_JIT_MISS;

    const uint16_t *src = (const uint16_t *)(V810_ROM1.off + (pc & ROM_MASK));
    jit_block *b = pd_jit_block_for_ra(&g_jit, pc, src);
    if (!b) return RV_JIT_MISS;
    if (b->pc_bytes == 0) return RV_JIT_MISS;        // stub: non-translatable lead op
    if (b->gen >= g_jit.flush_gen) return RV_JIT_MISS; // not yet I-cache-coherent

    // Block ABI: uint32_t blk(cpu_state *st); r0 = &v810_state (== &P_REG[0]),
    // returns the next V810 PC.
    uint32_t npc = ((uint32_t (*)(void *))b->entry)(&vb_state->v810_state);
    g_exec++;
    *cyc     = b->cycles;
    *last_op = b->last_op;
    return npc;
}
