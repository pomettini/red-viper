// Dispatcher glue between the V810 interpreter and the Thumb-2 JIT.
//
// The interpreter calls rv_jit_try() at the top of its loop for ROM PCs: if a
// translated straight-line block is ready it runs natively and the interpreter
// advances its PC/cycle locals by the returned amounts; otherwise the block is
// translated for next time (it only becomes executable after the per-frame
// rv_jit_frame_flush(), which batches the I-cache flush).
//
// This is intentionally a thin, gated layer (RV_JIT) so it can be A/B-measured
// against the pure interpreter.

#ifndef PD_JIT_RUN_H
#define PD_JIT_RUN_H

#include <stdint.h>
#include "pd_api.h"

// Allocate the code cache + block map. Safe to call once at startup; on
// failure the dispatcher stays disabled and rv_jit_try() is a no-op.
void rv_jit_init(PlaydateAPI *pd);

// Flush freshly translated blocks to the I-cache (once per frame).
void rv_jit_frame_flush(void);

// Sentinel returned by rv_jit_dispatch when there is no ready block (the block
// was translated for next time, or the lead op isn't translatable). It's an
// impossible V810 PC (odd, out of range), so it can't collide with a real one.
#define RV_JIT_MISS 0xFFFFFFFFu

// Run the ready basic block at v810_pc (a ROM address) and return the next V810
// PC, writing the block's cycle cost and final opcode. Returns RV_JIT_MISS if
// no ready block exists (caller interprets v810_pc this time).
uint32_t rv_jit_dispatch(uint32_t v810_pc, uint32_t *cyc, uint8_t *last_op);

#endif // PD_JIT_RUN_H
