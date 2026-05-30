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
    // Dispatcher metadata (filled by pd_jit_block_for). A block with
    // pc_bytes == 0 is a "stub" recording that this PC begins with a
    // non-translatable instruction, so the dispatcher won't retry translating.
    uint16_t   pc_bytes;   // V810 bytes the block advances PC by
    uint16_t   cycles;     // V810 cycles the block consumes (sum of opcycle[])
    uint8_t    last_op;    // opcode of the block's final instruction
    uint32_t   gen;        // code-gen epoch at translation (see jit_state.flush_gen)
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

    // Code-gen epoch. A block is safe to execute only once its emitted code
    // has been flushed to the I-cache. Translation stamps the block with the
    // current flush_gen; pd_jit_flush increments it. A block is executable iff
    // block.gen < flush_gen. This lets us batch clearICache() to once per frame
    // instead of once per translated block.
    uint32_t   flush_gen;
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

// Dispatcher entry point. Return the block for v810_pc, translating one from
// `src` (the native-order V810 instruction stream at v810_pc) on a miss. The
// returned block carries pc_bytes/cycles/last_op metadata. Returns NULL only if
// the cache/map is full and cannot be reset. A returned block may be a stub
// (pc_bytes == 0) meaning v810_pc starts with a non-translatable instruction —
// the caller should interpret it. Callers must still check block.gen <
// flush_gen before executing (freshly translated code isn't I-cache-coherent
// until the next pd_jit_flush).
jit_block *pd_jit_block_for(jit_state *j, uint32_t v810_pc, const uint16_t *src);

// Register-allocating, lazy-flag translator (Stage 1 of the real backend).
// Same contract as pd_jit_translate, but blocks take r0 = cpu_state* and keep
// V810 operands resident in ARM r4-r10 across the block, materialising PSW once
// at the end. Translates the move/arith/logic subset; stops at the first
// unsupported op. Validated by the t9 self-test before any game integration.
void *pd_jit_translate_ra(jit_state *j, const uint16_t *src, uint32_t start_pc,
                          int max_insns, int *out_count);

// Dispatcher lookup for the RA backend: returns the basic block at v810_pc,
// translating on a miss. The block's entry is uint32_t blk(cpu_state*) and
// returns the next V810 PC. A returned block with pc_bytes == 0 is a stub
// (untranslatable lead op — interpret it). Check gen < flush_gen before exec.
jit_block *pd_jit_block_for_ra(jit_state *j, uint32_t v810_pc, const uint16_t *src);

#endif // PD_JIT_H
