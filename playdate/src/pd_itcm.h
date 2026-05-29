// DTCM/ITCM acceleration for the hot V810 memory accessors.
//
// Technique (proven in the Beetle VB Playdate port): mark the hottest
// functions into a dedicated .itcm.rv linker section, then at frame start
// memcpy that section into a stack buffer — the Playdate stack lives in DTCM
// (zero-wait-state at 0x20000000) — and call the functions there through
// remapped pointers. Code fetched from DTCM never touches SDRAM or the I-cache.
//
// The interpreter calls the memory accessors through the g_rv_* function
// pointers. By default they point at the normal SDRAM mem_* functions (so the
// build is correct even with relocation disabled); on device, rv_itcm_relocate
// repoints them into the DTCM copy each frame.

#ifndef PD_ITCM_H
#define PD_ITCM_H

#include <stdint.h>
#include "vb_types.h"

typedef uint64_t (*rv_read_fn)(WORD addr);
typedef WORD     (*rv_write_fn)(WORD addr, WORD data);

extern rv_read_fn  g_rv_rbyte;
extern rv_read_fn  g_rv_rhword;
extern rv_read_fn  g_rv_rword;
extern rv_write_fn g_rv_wbyte;
extern rv_write_fn g_rv_whword;
extern rv_write_fn g_rv_wword;

// Interpreter routes its data accesses through these.
#define MEM_RBYTE   g_rv_rbyte
#define MEM_RHWORD  g_rv_rhword
#define MEM_RWORD   g_rv_rword
#define MEM_WBYTE   g_rv_wbyte
#define MEM_WHWORD  g_rv_whword
#define MEM_WWORD   g_rv_wword

// Initialise the pointers to the plain SDRAM mem_* functions.
void rv_itcm_init(void);

// Device only: copy the .itcm.rv section into the given (DTCM stack) buffer,
// barrier, and repoint g_rv_* into the copy. buf must be >= rv_itcm_size() and
// 32-byte aligned; it must stay live for the whole frame's emulation. No-op if
// the region doesn't fit. Returns 1 if relocation happened, 0 otherwise.
int rv_itcm_relocate(void *buf, uint32_t buf_size);

// Bytes needed for the relocation buffer (size of the .itcm.rv region).
uint32_t rv_itcm_size(void);

#endif // PD_ITCM_H
