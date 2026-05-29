#include <string.h>
#include "pd_jit.h"

static uint32_t round_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

bool pd_jit_init(jit_state *j, PlaydateAPI *pd, uint32_t code_bytes, uint32_t block_capacity) {
    memset(j, 0, sizeof(*j));
    j->pd = pd;

    // Code cache: heap (SDRAM) is executable on Playdate (confirmed by the
    // RWX probe). Align the base to a cache line for tidy flushing.
    j->code_base = (uint16_t *)pd->system->realloc(NULL, code_bytes);
    if (!j->code_base) return false;
    j->code_pos = j->code_base;
    j->code_end = j->code_base + code_bytes / sizeof(uint16_t);

    uint32_t cap = round_pow2(block_capacity);
    j->blocks = (jit_block *)pd->system->realloc(NULL, cap * sizeof(jit_block));
    if (!j->blocks) {
        pd->system->realloc(j->code_base, 0);
        j->code_base = NULL;
        return false;
    }
    memset(j->blocks, 0, cap * sizeof(jit_block));
    j->block_mask = cap - 1;
    j->block_count = 0;
    j->dirty = false;
    return true;
}

void pd_jit_free(jit_state *j) {
    if (j->code_base) j->pd->system->realloc(j->code_base, 0);
    if (j->blocks)    j->pd->system->realloc(j->blocks, 0);
    j->code_base = NULL;
    j->blocks = NULL;
}

void pd_jit_reset(jit_state *j) {
    j->code_pos = j->code_base;
    memset(j->blocks, 0, (j->block_mask + 1) * sizeof(jit_block));
    j->block_count = 0;
    j->dirty = false;
}

void pd_jit_flush(jit_state *j) {
    if (j->dirty) {
        j->pd->system->clearICache();
        j->dirty = false;
    }
}

// V810 instructions are halfword-aligned, so the low bit of the PC carries no
// information; mix the upper bits down for a better hash spread.
static inline uint32_t pc_hash(uint32_t pc) {
    pc >>= 1;
    pc ^= pc >> 13;
    return pc;
}

void *pd_jit_lookup(jit_state *j, uint32_t v810_pc) {
    uint32_t i = pc_hash(v810_pc) & j->block_mask;
    for (;;) {
        jit_block *b = &j->blocks[i];
        if (b->v810_pc == v810_pc && b->entry) return b->entry;
        if (b->entry == NULL) return NULL; // empty slot terminates the probe
        i = (i + 1) & j->block_mask;
    }
}

uint16_t *pd_jit_begin_block(jit_state *j, uint32_t v810_pc) {
    // Keep the map below 3/4 full so probe chains stay short.
    if (j->block_count > (j->block_mask + 1) * 3 / 4) return NULL;
    // Leave generous headroom for one block's worth of emission.
    if (pd_jit_code_free(j) < 1024) return NULL;

    // Align entry to a 4-byte boundary (32-bit Thumb-2 fetch friendliness).
    if (((uintptr_t)j->code_pos & 2) != 0) *j->code_pos++ = 0xBF00; // nop pad

    uint32_t i = pc_hash(v810_pc) & j->block_mask;
    for (;;) {
        jit_block *b = &j->blocks[i];
        if (b->entry == NULL) {
            b->v810_pc = v810_pc;
            b->entry = (void *)((uintptr_t)j->code_pos | 1u); // Thumb bit
            j->block_count++;
            j->dirty = true;
            return j->code_pos;
        }
        if (b->v810_pc == v810_pc) {
            // Already present — overwrite entry to point at fresh code.
            b->entry = (void *)((uintptr_t)j->code_pos | 1u);
            j->dirty = true;
            return j->code_pos;
        }
        i = (i + 1) & j->block_mask;
    }
}
