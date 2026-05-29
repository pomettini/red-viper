// V810 -> Thumb-2 dynarec: code cache + block map.
//
// This is the infrastructure layer beneath block translation: a heap-backed
// executable code buffer (proven executable on device), a bump-pointer
// allocator with an I-cache flush, and an open-addressed map from V810 PC to
// translated code entry point. Translation itself (pd_jit_translate) lands in
// a later step; this layer is independently testable.

#ifndef PD_JIT_H
#define PD_JIT_H

#include <stdint.h>
#include <stdbool.h>
#include "pd_api.h"

// A translated block: native Thumb-2 entry (with the Thumb bit already set,
// ready to call) plus bookkeeping.
typedef struct {
    uint32_t   v810_pc;    // V810 address this block starts at (0 = empty slot)
    void      *entry;      // callable native entry (Thumb bit set)
} jit_block;

typedef struct {
    PlaydateAPI *pd;

    // Executable code cache (heap / SDRAM).
    uint16_t *code_base;   // start of buffer
    uint16_t *code_pos;    // bump cursor
    uint16_t *code_end;    // one past the end
    bool      dirty;       // emitted since last flush?

    // Open-addressed PC -> block map.
    jit_block *blocks;
    uint32_t   block_mask; // capacity-1 (capacity is a power of two)
    uint32_t   block_count;
} jit_state;

// Allocate the code cache (code_bytes) and block map (block_capacity entries,
// rounded up to a power of two). Returns false on allocation failure.
bool pd_jit_init(jit_state *j, PlaydateAPI *pd, uint32_t code_bytes, uint32_t block_capacity);
void pd_jit_free(jit_state *j);

// Drop all translations (e.g. on cache exhaustion). Does not free memory.
void pd_jit_reset(jit_state *j);

// Flush the I-cache if anything was emitted since the last flush. Call once
// after a batch of translation, before executing freshly emitted code.
void pd_jit_flush(jit_state *j);

// Look up a translated block for v810_pc, or NULL if not translated.
void *pd_jit_lookup(jit_state *j, uint32_t v810_pc);

// Reserve the current code cursor as the entry for a new block at v810_pc and
// return a uint16_t** cursor to emit into. Returns NULL if the cache or block
// map is full (caller should pd_jit_reset and retry, or fall back). The block
// is registered immediately; emit into *out_cursor, then nothing else needed.
uint16_t *pd_jit_begin_block(jit_state *j, uint32_t v810_pc);

// Bytes of headroom left in the code cache.
static inline uint32_t pd_jit_code_free(const jit_state *j) {
    return (uint32_t)((j->code_end - j->code_pos) * sizeof(uint16_t));
}

// Translate a run of V810 register-move instructions (MOV, MOV_I, MOVEA,
// MOVHI) starting at start_pc, reading the V810 instruction stream from src
// (halfword-addressed, native order). Stops at the first unsupported opcode
// or after max_insns. Emits a block ending in `bx lr`; the block is called
// as void blk(uint32_t *p_reg) with r0 = pointer to the 32-entry V810
// general register file. Returns the callable entry (Thumb bit set) or NULL
// on cache-full, and writes the number of V810 instructions translated to
// *out_count. Does NOT flush the I-cache; caller batches and calls
// pd_jit_flush before executing.
void *pd_jit_translate(jit_state *j, const uint16_t *src, uint32_t start_pc,
                       int max_insns, int *out_count);

#endif // PD_JIT_H
