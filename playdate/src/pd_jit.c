#include <string.h>
#include "pd_jit.h"
#include "t2_emit.h"
#include "pd_itcm.h"   // g_rv_* memory accessor pointers (call targets)

// V810 opcodes the translator handles (see include/v810_opt.h).
#define OP_MOV    0x00
#define OP_ADD    0x01
#define OP_SUB    0x02
#define OP_CMP    0x03
#define OP_OR     0x0C
#define OP_AND    0x0D
#define OP_XOR    0x0E
#define OP_MOV_I  0x10
#define OP_ADD_I  0x11
#define OP_CMP_I  0x13
#define OP_MOVEA  0x28
#define OP_ADDI   0x29
#define OP_ORI    0x2C
#define OP_ANDI   0x2D
#define OP_XORI   0x2E
#define OP_MOVHI  0x2F
#define OP_JMP    0x06   // small op: PC = reg1 (register-indirect)
#define OP_JR     0x2A   // long op: PC += disp26
#define OP_JAL    0x2B   // long op: r31 = next; PC += disp26
// Loads (sign-extended), zero-extended loads, and stores. instr2 = disp16.
#define OP_LD_B   0x30
#define OP_LD_H   0x31
#define OP_LD_W   0x33
#define OP_ST_B   0x34
#define OP_ST_H   0x35
#define OP_ST_W   0x37
#define OP_IN_B   0x38
#define OP_IN_H   0x39
#define OP_IN_W   0x3B
#define OP_OUT_B  0x3C
#define OP_OUT_H  0x3D
#define OP_OUT_W  0x3F

// cpu_state byte offsets (P_REG first, then S_REG[32], ...). PSW = S_REG[5].
#define OFF_PREG(n)  ((uint16_t)((n) * 4))
#define OFF_PSW      ((uint16_t)(128 + 5 * 4)) // 148

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
        j->flush_gen++;   // blocks translated before now are now executable
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

// Emit "r1 = V810 reg" : a literal 0 for r0 (the interpreter never reads
// P_REG[0]), else a load from the register file at offset reg*4.
static inline void emit_load_reg(uint16_t **out, int armreg, int v810reg) {
    if (v810reg == 0) t2_movw(out, armreg, 0);
    else              t2_ldr_imm(out, armreg, 0 /*r0=base*/, OFF_PREG(v810reg));
}

// Emit the V810 Z/S flag update for a logic-style op (OR/AND/XOR/...), which
// set Z and S and clear OV, leaving CY untouched:
//   PSW = (PSW & ~7) | (res==0) | ((res<0)<<1)
// Computes Z branchlessly via CLZ (clz(0)=32 -> >>5 = 1; nonzero -> 0) and S
// via a logical shift. res must be in `res_reg`; clobbers s1, s2 (scratch).
// res_reg is left intact for the caller's store.
static void emit_flags_zs(uint16_t **out, int res_reg, int s1, int s2) {
    t2_clz(out, s1, res_reg);          // s1 = clz(res)
    t2_lsr_imm(out, s1, s1, 5);        // s1 = Z  (1 iff res==0)
    t2_lsr_imm(out, s2, res_reg, 31);  // s2 = S  (res>>31)
    t2_lsl_imm(out, s2, s2, 1);        // s2 = S<<1
    t2_orr_reg(out, s1, s1, s2);       // s1 = Z | (S<<1)
    t2_ldr_imm(out, s2, 0, OFF_PSW);   // s2 = PSW
    t2_bic_imm(out, s2, s2, 0x7);      // clear Z,S,OV
    t2_orr_reg(out, s2, s2, s1);       // merge new Z,S
    t2_str_imm(out, s2, 0, OFF_PSW);   // PSW = ...
}

// Pack the ARM condition flags (set by a preceding adds/subs, read from APSR)
// into the V810 PSW low nibble: Z(bit0)<-ARM Z, S(bit1)<-ARM N, OV(bit2)<-ARM
// V, CY(bit3)<-ARM C. For subtraction V810 CY is the borrow = !ARM_C, so pass
// invert_carry=1. Uses r1,r2,r12 scratch; r0=state base. (V810 ADD/SUB set all
// of Z/S/OV/CY; matches interpreter's `(PSW & ~0xf) | z | s<<1 | ov<<2 | cy<<3`.)
static void emit_pack_nzcv(uint16_t **out, int invert_carry) {
    t2_mrs_apsr(out, 12);                                   // r12 = NZCV (bits 31..28)
    t2_ubfx(out, 1, 12, 30, 1);                             // r1  = Z            (-> bit0)
    t2_ubfx(out, 2, 12, 31, 1); t2_lsl_imm(out, 2, 2, 1); t2_orr_reg(out, 1, 1, 2); // | S<<1
    t2_ubfx(out, 2, 12, 28, 1); t2_lsl_imm(out, 2, 2, 2); t2_orr_reg(out, 1, 1, 2); // | OV<<2
    t2_ubfx(out, 2, 12, 29, 1);                             // r2 = C
    if (invert_carry) t2_eor_imm(out, 2, 2, 1);             // r2 = !C  (borrow)
    t2_lsl_imm(out, 2, 2, 3); t2_orr_reg(out, 1, 1, 2);     // | CY<<3
    t2_ldr_imm(out, 2, 0, OFF_PSW);
    t2_bic_imm(out, 2, 2, 0xf);                             // clear Z,S,OV,CY
    t2_orr_reg(out, 2, 2, 1);
    t2_str_imm(out, 2, 0, OFF_PSW);
}

void *pd_jit_translate(jit_state *j, const uint16_t *src, uint32_t start_pc,
                       int max_insns, int *out_count) {
    uint16_t *out = pd_jit_begin_block(j, start_pc);
    if (!out) { if (out_count) *out_count = 0; return NULL; }
    void *entry = (void *)((uintptr_t)out | 1u);

    const uint16_t *s = src;
    int n = 0;
    while (n < max_insns) {
        uint16_t instr = s[0];
        int op   = instr >> 10;
        int reg2 = (instr >> 5) & 31;
        int reg1 = instr & 31;

        if (op == OP_MOV) {
            emit_load_reg(&out, 1, reg1);
            t2_str_imm(&out, 1, 0, (uint16_t)(reg2 * 4));
            s += 1; n++;
        } else if (op == OP_MOV_I) {
            // 5-bit immediate, sign-extended.
            int32_t imm = (reg1 & 0x10) ? (int32_t)(reg1 | 0xFFFFFFF0u) : reg1;
            t2_mov32(&out, 1, (uint32_t)imm);
            t2_str_imm(&out, 1, 0, (uint16_t)(reg2 * 4));
            s += 1; n++;
        } else if (op == OP_MOVEA) {
            int32_t imm = (int32_t)(int16_t)s[1];
            emit_load_reg(&out, 1, reg1);
            t2_mov32(&out, 2, (uint32_t)imm);
            t2_add_reg(&out, 1, 1, 2);
            t2_str_imm(&out, 1, 0, (uint16_t)(reg2 * 4));
            s += 2; n++;
        } else if (op == OP_MOVHI) {
            uint32_t imm = ((uint32_t)s[1]) << 16;
            emit_load_reg(&out, 1, reg1);
            t2_mov32(&out, 2, imm);
            t2_add_reg(&out, 1, 1, 2);
            t2_str_imm(&out, 1, 0, (uint16_t)(reg2 * 4));
            s += 2; n++;
        } else if (op == OP_ADD || op == OP_SUB || op == OP_CMP) {
            // reg2 = reg2 +/- reg1 (CMP: flags only). Sets Z,S,OV,CY.
            emit_load_reg(&out, 1, reg2);
            emit_load_reg(&out, 2, reg1);
            if (op == OP_ADD) {
                t2_adds_reg(&out, 3, 1, 2);
                t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
                emit_pack_nzcv(&out, 0);
            } else { // SUB or CMP
                t2_subs_reg(&out, 3, 1, 2);
                if (op == OP_SUB) t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
                emit_pack_nzcv(&out, 1);
            }
            s += 1; n++;
        } else if (op == OP_ADD_I || op == OP_CMP_I) {
            // imm5, sign-extended. ADD_I: reg2 += imm. CMP_I: reg2 - imm flags.
            int32_t imm = (reg1 & 0x10) ? (int32_t)(reg1 | 0xFFFFFFF0u) : reg1;
            emit_load_reg(&out, 1, reg2);
            t2_mov32(&out, 2, (uint32_t)imm);
            if (op == OP_ADD_I) {
                t2_adds_reg(&out, 3, 1, 2);
                t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
                emit_pack_nzcv(&out, 0);
            } else {
                t2_subs_reg(&out, 3, 1, 2);
                emit_pack_nzcv(&out, 1);
            }
            s += 1; n++;
        } else if (op == OP_ADDI) {
            // reg2 = reg1 + sign_extend16(imm). Sets Z,S,OV,CY.
            int32_t imm = (int32_t)(int16_t)s[1];
            emit_load_reg(&out, 1, reg1);
            t2_mov32(&out, 2, (uint32_t)imm);
            t2_adds_reg(&out, 3, 1, 2);
            t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
            emit_pack_nzcv(&out, 0);
            s += 2; n++;
        } else if (op == OP_OR || op == OP_AND || op == OP_XOR) {
            // reg2 = reg2 OP reg1 ; sets Z,S, clears OV
            emit_load_reg(&out, 1, reg2);
            emit_load_reg(&out, 2, reg1);
            if (op == OP_OR)       t2_orr_reg(&out, 3, 1, 2);
            else if (op == OP_AND) t2_and_reg(&out, 3, 1, 2);
            else                   t2_eor_reg(&out, 3, 1, 2);
            emit_flags_zs(&out, 3, 1, 2);
            t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
            s += 1; n++;
        } else if (op == OP_ORI || op == OP_ANDI || op == OP_XORI) {
            // reg2 = reg1 OP zero_extend16(imm) ; sets Z,S, clears OV
            uint32_t imm = (uint32_t)s[1];
            emit_load_reg(&out, 1, reg1);
            t2_mov32(&out, 2, imm);
            if (op == OP_ORI)       t2_orr_reg(&out, 3, 1, 2);
            else if (op == OP_ANDI) t2_and_reg(&out, 3, 1, 2);
            else                    t2_eor_reg(&out, 3, 1, 2);
            emit_flags_zs(&out, 3, 1, 2);
            t2_str_imm(&out, 3, 0, OFF_PREG(reg2));
            s += 2; n++;
        } else {
            break; // unsupported op ends the block
        }
    }

    t2_bx(&out, 14); // bx lr
    j->code_pos = out;
    if (out_count) *out_count = n;
    return entry;
}

// V810 per-opcode cycle table (source/common/v810_cpu.c). Used to compute a
// block's cycle cost so the dispatcher can advance the interpreter's clock.
extern const unsigned char opcycle[];

// Like pd_jit_lookup but returns the map entry so the dispatcher can read/write
// its metadata. NULL if not present.
static jit_block *pd_jit_find(jit_state *j, uint32_t pc) {
    uint32_t i = pc_hash(pc) & j->block_mask;
    for (;;) {
        jit_block *b = &j->blocks[i];
        if (b->entry && b->v810_pc == pc) return b;
        if (b->entry == NULL) return NULL;
        i = (i + 1) & j->block_mask;
    }
}

jit_block *pd_jit_block_for(jit_state *j, uint32_t pc, const uint16_t *src) {
    jit_block *b = pd_jit_find(j, pc);
    if (b) return b;

    // Cap the run length so one block can't overflow the code cursor's
    // headroom (begin_block guarantees ~1 KB; worst-case arith ≈ 36 B/insn).
    int count = 0;
    void *entry = pd_jit_translate(j, src, pc, 16, &count);
    if (!entry) {
        pd_jit_reset(j);                 // cache/map full: start over once
        entry = pd_jit_translate(j, src, pc, 16, &count);
        if (!entry) return NULL;
    }
    b = pd_jit_find(j, pc);
    if (!b) return NULL;

    // Walk the translated run to total its PC advance and cycle cost. The size
    // rule matches the translator: 32-bit (4-byte) ops are >= 0x28, else 2-byte.
    uint32_t bytes = 0, cyc = 0;
    uint8_t last_op = 0;
    const uint16_t *s = src;
    for (int k = 0; k < count; k++) {
        uint8_t op = (uint8_t)(s[0] >> 10);
        last_op = op;
        cyc += opcycle[op];
        int hw = (op >= 0x28) ? 2 : 1;   // halfwords this instruction occupies
        s += hw;
        bytes += (uint32_t)hw * 2;
    }
    b->pc_bytes = (uint16_t)bytes;
    b->cycles   = (uint16_t)cyc;
    b->last_op  = last_op;
    b->gen      = j->flush_gen;
    return b;
}

// ===========================================================================
// Stage 1 of the real backend: register-allocating, lazy-flag block translator
// ===========================================================================
//
// ARM register map inside a translated block:
//   r11          = base pointer to cpu_state (== &P_REG[0])
//   r4..r10      = V810 register allocation pool (7 slots, allocated on demand)
//   r3           = constant 0 (covers reads of V810 r0)
//   r0,r1,r2,r12 = scratch (immediates, flag packing)
// Block ABI: void blk(cpu_state *st); st passed in r0. The prologue saves the
// callee-saved pool + lr and moves the base into r11; the epilogue stores back
// dirty pooled regs and returns via `pop {..,pc}`.
//
// V810 flags are tracked lazily. Every flag-setting op leaves its result in ARM
// CPSR (adds/subs/ands/orrs/eors); only the final flag-setting op of a
// straight-line block is observable, so PSW is materialised exactly once in the
// epilogue. This removes both the per-operand ldr/str (register allocation) and
// the per-op flag packing (lazy) that made the first dispatcher a loss.

#define RA_BASE    11
#define RA_ZERO    3
#define RA_POOL_LO 4
#define RA_POOL_HI 10

enum { FL_NONE = 0, FL_ADD, FL_SUB, FL_LOGIC };

// PSW lives at S_REG[5]; see OFF_PSW. Pack helpers parameterised on the base
// register (r11 here instead of r0 used by the straight-line translator).
static void ra_pack_nzcv(uint16_t **out, int base, int invert_carry) {
    t2_mrs_apsr(out, 12);
    t2_ubfx(out, 1, 12, 30, 1);                                             // Z
    t2_ubfx(out, 2, 12, 31, 1); t2_lsl_imm(out, 2, 2, 1); t2_orr_reg(out, 1, 1, 2); // |S<<1
    t2_ubfx(out, 2, 12, 28, 1); t2_lsl_imm(out, 2, 2, 2); t2_orr_reg(out, 1, 1, 2); // |OV<<2
    t2_ubfx(out, 2, 12, 29, 1);                                             // C
    if (invert_carry) t2_eor_imm(out, 2, 2, 1);
    t2_lsl_imm(out, 2, 2, 3); t2_orr_reg(out, 1, 1, 2);                     // |CY<<3
    t2_ldr_imm(out, 2, base, OFF_PSW);
    t2_bic_imm(out, 2, 2, 0xf);
    t2_orr_reg(out, 2, 2, 1);
    t2_str_imm(out, 2, base, OFF_PSW);
}

// Logic flags: Z,S from CPSR, OV cleared, CY left as-is in PSW.
static void ra_pack_logic(uint16_t **out, int base) {
    t2_mrs_apsr(out, 12);
    t2_ubfx(out, 1, 12, 30, 1);                                             // Z
    t2_ubfx(out, 2, 12, 31, 1); t2_lsl_imm(out, 2, 2, 1); t2_orr_reg(out, 1, 1, 2); // |S<<1
    t2_ldr_imm(out, 2, base, OFF_PSW);
    t2_bic_imm(out, 2, 2, 0x7);                                            // clear Z,S,OV (keep CY)
    t2_orr_reg(out, 2, 2, 1);
    t2_str_imm(out, 2, base, OFF_PSW);
}

// Allocate/find the ARM reg holding V810 reg v, loading it on first use.
// Returns -1 if the pool is exhausted.
static int ra_use(uint16_t **out, int8_t *armof, int *next, int v) {
    if (v == 0) return RA_ZERO;
    if (armof[v] >= 0) return armof[v];
    if (*next > RA_POOL_HI) return -1;
    int a = (*next)++;
    armof[v] = (int8_t)a;
    t2_ldr_imm(out, a, RA_BASE, (uint16_t)(v * 4));
    return a;
}

// Allocate the ARM reg that will hold a freshly written V810 reg v (no load).
// v==0 returns a throwaway scratch (writes to r0 are discarded). -1 if full.
static int ra_def(int8_t *armof, int *next, uint8_t *dirty, int v) {
    if (v == 0) return 12;
    if (armof[v] >= 0) { dirty[v] = 1; return armof[v]; }
    if (*next > RA_POOL_HI) return -1;
    int a = (*next)++;
    armof[v] = (int8_t)a;
    dirty[v] = 1;
    return a;
}

// Materialise the pending V810 flags into PSW (call before anything that
// clobbers ARM CPSR — i.e. a memory-accessor call — and at block exit).
static void ra_flush(uint16_t **out, int kind) {
    if (kind == FL_ADD)        ra_pack_nzcv(out, RA_BASE, 0);
    else if (kind == FL_SUB)   ra_pack_nzcv(out, RA_BASE, 1);
    else if (kind == FL_LOGIC) ra_pack_logic(out, RA_BASE);
}

// Runtime address of the g_rv_* accessor pointer for a load/store opcode. The
// block loads *through* this (the pointers are repointed into DTCM each frame),
// so it always calls the current accessor.
static uint32_t rv_accessor_addr(int op) {
    switch (op) {
        case OP_LD_B: case OP_IN_B:  return (uint32_t)(uintptr_t)&g_rv_rbyte;
        case OP_LD_H: case OP_IN_H:  return (uint32_t)(uintptr_t)&g_rv_rhword;
        case OP_LD_W: case OP_IN_W:  return (uint32_t)(uintptr_t)&g_rv_rword;
        case OP_ST_B: case OP_OUT_B: return (uint32_t)(uintptr_t)&g_rv_wbyte;
        case OP_ST_H: case OP_OUT_H: return (uint32_t)(uintptr_t)&g_rv_whword;
        default:                     return (uint32_t)(uintptr_t)&g_rv_wword;
    }
}

// Emit the V810 branch-condition test for the 4-bit cond field, leaving a 0/1
// boolean in r2. Reads PSW into r1 (clobbers r1,r2,r12). Mirrors get_cond():
// cond = code&7 selects the bits, code&8 inverts.
static void ra_emit_cond(uint16_t **out, int cond4) {
    t2_ldr_imm(out, 1, RA_BASE, OFF_PSW);   // r1 = PSW
    switch (cond4 & 7) {
        case 0: t2_ubfx(out, 2, 1, 2, 1); break;                 // OV
        case 1: t2_ubfx(out, 2, 1, 3, 1); break;                 // CY
        case 2: t2_ubfx(out, 2, 1, 0, 1); break;                 // Z
        case 3: t2_ubfx(out, 2, 1, 3, 1); t2_ubfx(out, 12, 1, 0, 1);
                t2_orr_reg(out, 2, 2, 12); break;                // CY|Z
        case 4: t2_ubfx(out, 2, 1, 1, 1); break;                 // S
        case 5: t2_movw(out, 2, 1); break;                       // always
        case 6: t2_ubfx(out, 2, 1, 2, 1); t2_ubfx(out, 12, 1, 1, 1);
                t2_eor_reg(out, 2, 2, 12); break;                // OV ^ S
        case 7: t2_ubfx(out, 2, 1, 2, 1); t2_ubfx(out, 12, 1, 1, 1);
                t2_eor_reg(out, 2, 2, 12);
                t2_ubfx(out, 12, 1, 0, 1); t2_orr_reg(out, 2, 2, 12); break; // Z|(OV^S)
    }
    if (cond4 & 8) t2_eor_imm(out, 2, 2, 1);   // invert
}

// Diagnostic: total RA-block translations since boot (one per cache miss).
uint32_t g_ra_xlate_count = 0;

void *pd_jit_translate_ra(jit_state *j, const uint16_t *src, uint32_t start_pc,
                          int max_insns, int *out_count) {
    g_ra_xlate_count++;
    uint16_t *out = pd_jit_begin_block(j, start_pc);
    if (!out) { if (out_count) *out_count = 0; return NULL; }
    void *entry = (void *)((uintptr_t)out | 1u);

    int8_t  armof[32];
    uint8_t dirty[32];
    for (int i = 0; i < 32; i++) { armof[i] = -1; dirty[i] = 0; }
    int next = RA_POOL_LO;
    int fl_kind = FL_NONE;
    int fl_dirty = 0;       // a flag-setting op's result is live in CPSR, unpacked
    uint32_t bytes = 0;     // V810 bytes translated so far (PC advance)
    int term_pc_set = 0;    // a branch/jump already wrote the next PC into r0

    // Prologue: save the regs we'll use + lr, base into r11, r3 = 0. The push
    // register-list is back-patched at the epilogue once we know how many pool
    // regs (r4..next-1) were actually allocated — small blocks then save far
    // fewer registers.
    uint16_t *push_loc = out;
    t2_push_w(&out, 0x4FF0);          // placeholder; patched below
    t2_mov_reg(&out, RA_BASE, 0);     // r11 = st
    t2_movw(&out, RA_ZERO, 0);        // r3  = 0

    const uint16_t *s = src;
    int n = 0;
    while (n < max_insns) {
        uint16_t instr = s[0];
        int op   = instr >> 10;
        int reg2 = (instr >> 5) & 31;
        int reg1 = instr & 31;
        int adv  = (op >= 0x28) ? 2 : 1;   // halfwords
        int a1, a2, ad;

        // --- control flow: terminate the block, computing the next PC into r0 ---
        if (op >= 0x20 && op < 0x28) {
            // Bcond: PC-relative, 9-bit signed disp; target/fallthrough are
            // compile-time constants. Evaluate the condition from PSW at runtime
            // and select branchlessly: r0 = fallthrough + (cond ? delta : 0).
            if (fl_dirty) { ra_flush(&out, fl_kind); fl_dirty = 0; }
            int raw  = instr & 0x1ff;
            int disp = (raw & 0x100) ? raw - 0x200 : raw;
            uint32_t branch_pc = start_pc + bytes;
            uint32_t target = branch_pc + (uint32_t)disp;
            uint32_t fallth = branch_pc + 2;
            ra_emit_cond(&out, (instr >> 9) & 0xf);
            t2_mov32(&out, 0, fallth);
            t2_mov32(&out, 12, target - fallth);
            t2_lsl_imm(&out, 2, 2, 31);
            t2_asr_imm(&out, 2, 2, 31);          // r2 = cond ? -1 : 0
            t2_and_reg(&out, 12, 12, 2);
            t2_add_reg(&out, 0, 0, 12);
            n++; bytes += 2; term_pc_set = 1;
            goto done;
        }
        if (op == OP_JR || op == OP_JAL) {
            if (fl_dirty) { ra_flush(&out, fl_kind); fl_dirty = 0; }
            int32_t disp = (int32_t)(((uint32_t)instr << 16) | s[1]);
            if (disp & 0x02000000) disp |= 0xfc000000; else disp &= ~0xfc000000u;
            uint32_t branch_pc = start_pc + bytes;
            if (op == OP_JAL) {
                uint32_t ret = branch_pc + 4;
                if (armof[31] >= 0) { t2_mov32(&out, armof[31], ret); dirty[31] = 1; }
                else { t2_mov32(&out, 12, ret); t2_str_imm(&out, 12, RA_BASE, (uint16_t)(31 * 4)); }
            }
            t2_mov32(&out, 0, branch_pc + (uint32_t)disp);
            n++; bytes += 4; term_pc_set = 1;
            goto done;
        }
        if (op == OP_JMP) {
            // register-indirect: next PC = reg1 (0 if r0). If the pool is full we
            // fall out and let the interpreter handle this JMP.
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            if (fl_dirty) { ra_flush(&out, fl_kind); fl_dirty = 0; }
            t2_mov_reg(&out, 0, a1);
            n++; bytes += 2; term_pc_set = 1;
            goto done;
        }

        switch (op) {
        case OP_MOV:
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov_reg(&out, ad, a1);
            break;
        case OP_MOV_I: {
            int32_t imm = (reg1 & 0x10) ? (int32_t)(reg1 | 0xFFFFFFF0u) : reg1;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov32(&out, ad, (uint32_t)imm);
            break;
        }
        case OP_MOVEA: {
            int32_t imm = (int32_t)(int16_t)s[1];
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov32(&out, 12, (uint32_t)imm);
            t2_add_reg(&out, ad, a1, 12);
            break;
        }
        case OP_MOVHI: {
            uint32_t imm = ((uint32_t)s[1]) << 16;
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov32(&out, 12, imm);
            t2_add_reg(&out, ad, a1, 12);
            break;
        }
        case OP_ADD:
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            dirty[reg2 ? reg2 : 0] = (reg2 != 0);
            t2_adds_reg(&out, (reg2 ? a2 : 12), a2, a1);
            fl_kind = FL_ADD; fl_dirty = 1;
            break;
        case OP_SUB:
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            dirty[reg2 ? reg2 : 0] = (reg2 != 0);
            t2_subs_reg(&out, (reg2 ? a2 : 12), a2, a1);
            fl_kind = FL_SUB; fl_dirty = 1;
            break;
        case OP_CMP:
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            t2_subs_reg(&out, 12, a2, a1);   // flags only
            fl_kind = FL_SUB; fl_dirty = 1;
            break;
        case OP_ADD_I: {
            int32_t imm = (reg1 & 0x10) ? (int32_t)(reg1 | 0xFFFFFFF0u) : reg1;
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            t2_mov32(&out, 12, (uint32_t)imm);
            dirty[reg2 ? reg2 : 0] = (reg2 != 0);
            t2_adds_reg(&out, (reg2 ? a2 : 12), a2, 12);
            fl_kind = FL_ADD; fl_dirty = 1;
            break;
        }
        case OP_CMP_I: {
            int32_t imm = (reg1 & 0x10) ? (int32_t)(reg1 | 0xFFFFFFF0u) : reg1;
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            t2_mov32(&out, 12, (uint32_t)imm);
            t2_subs_reg(&out, 12, a2, 12);   // flags only
            fl_kind = FL_SUB; fl_dirty = 1;
            break;
        }
        case OP_ADDI: {
            int32_t imm = (int32_t)(int16_t)s[1];
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov32(&out, 12, (uint32_t)imm);
            t2_adds_reg(&out, ad, a1, 12);
            fl_kind = FL_ADD; fl_dirty = 1;
            break;
        }
        case OP_OR: case OP_AND: case OP_XOR:
            a2 = ra_use(&out, armof, &next, reg2); if (a2 < 0) goto done;
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            if (op == OP_OR)       t2_orrs_reg(&out, ad, a2, a1);
            else if (op == OP_AND) t2_ands_reg(&out, ad, a2, a1);
            else                   t2_eors_reg(&out, ad, a2, a1);
            fl_kind = FL_LOGIC; fl_dirty = 1;
            break;
        case OP_ORI: case OP_ANDI: case OP_XORI: {
            uint32_t imm = (uint32_t)s[1];
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            t2_mov32(&out, 12, imm);
            if (op == OP_ORI)       t2_orrs_reg(&out, ad, a1, 12);
            else if (op == OP_ANDI) t2_ands_reg(&out, ad, a1, 12);
            else                    t2_eors_reg(&out, ad, a1, 12);
            fl_kind = FL_LOGIC; fl_dirty = 1;
            break;
        }
        case OP_LD_B: case OP_LD_H: case OP_LD_W:
        case OP_IN_B: case OP_IN_H: case OP_IN_W: {
            int32_t disp = (int32_t)(int16_t)s[1];
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;
            ad = ra_def(armof, &next, dirty, reg2); if (ad < 0) goto done;
            if (fl_dirty) { ra_flush(&out, fl_kind); fl_dirty = 0; } // call clobbers CPSR
            // r0 = reg1 + disp
            t2_mov32(&out, 12, (uint32_t)disp);
            t2_add_reg(&out, 0, a1, 12);
            // r12 = *(g_rv_read), call
            t2_mov32(&out, 12, rv_accessor_addr(op));
            t2_ldr_imm(&out, 12, 12, 0);
            t2_blx(&out, 12);
            t2_movw(&out, RA_ZERO, 0);   // call clobbered r3 (the zero reg)
            // result (low 32 bits in r0) -> dest, with the op's extension
            switch (op) {
                case OP_LD_B: t2_sxtb(&out, ad, 0); break;
                case OP_IN_B: t2_uxtb(&out, ad, 0); break;
                case OP_LD_H: t2_sxth(&out, ad, 0); break;
                case OP_IN_H: t2_uxth(&out, ad, 0); break;
                default:      t2_mov_reg(&out, ad, 0); break; // LD_W / IN_W
            }
            break;
        }
        case OP_ST_B: case OP_ST_H: case OP_ST_W:
        case OP_OUT_B: case OP_OUT_H: case OP_OUT_W: {
            int32_t disp = (int32_t)(int16_t)s[1];
            a1 = ra_use(&out, armof, &next, reg1); if (a1 < 0) goto done;          // addr base
            int ad2 = ra_use(&out, armof, &next, reg2); if (ad2 < 0) goto done;    // data
            if (fl_dirty) { ra_flush(&out, fl_kind); fl_dirty = 0; }
            t2_mov32(&out, 12, (uint32_t)disp);
            t2_add_reg(&out, 0, a1, 12);   // r0 = addr
            t2_mov_reg(&out, 1, ad2);      // r1 = data
            t2_mov32(&out, 12, rv_accessor_addr(op));
            t2_ldr_imm(&out, 12, 12, 0);
            t2_blx(&out, 12);
            t2_movw(&out, RA_ZERO, 0);     // restore zero reg
            break;
        }
        default:
            goto done;   // unsupported op terminates the block
        }
        s += adv;
        n++;
        bytes += (uint32_t)adv * 2;
    }
done:
    // Epilogue: materialise the final flags (if still live), set the next PC if
    // no branch did, store dirty pooled regs, and return next-PC in r0.
    if (fl_dirty) ra_flush(&out, fl_kind);
    if (!term_pc_set) t2_mov32(&out, 0, start_pc + bytes);

    for (int v = 1; v < 32; v++)
        if (armof[v] >= 0 && dirty[v])
            t2_str_imm(&out, armof[v], RA_BASE, (uint16_t)(v * 4));

    // Save/restore only r11 (base) + the pool regs r4..next-1 that were used.
    uint16_t regmask = (uint16_t)(1 << RA_BASE);   // r11 always
    for (int r = RA_POOL_LO; r < next; r++) regmask |= (uint16_t)(1 << r);
    push_loc[1] = regmask | 0x4000;       // patch push: + lr
    t2_pop_w(&out, regmask | 0x8000);     // pop: + pc -> returns r0 (next V810 PC)
    j->code_pos = out;
    if (out_count) *out_count = n;
    return entry;
}

// Dispatcher entry for the real backend: look up (or RA-translate) a basic block
// at pc. The returned block's `entry` is called as uint32_t blk(cpu_state*) and
// returns the next V810 PC. cycles = sum of opcycle[] over the block; pc_bytes
// is the byte span (0 ⇒ the lead op was untranslatable: a stub, caller should
// interpret). last_op is the final instruction's opcode.
jit_block *pd_jit_block_for_ra(jit_state *j, uint32_t pc, const uint16_t *src) {
    jit_block *b = pd_jit_find(j, pc);
    if (b) return b;

    int count = 0;
    void *entry = pd_jit_translate_ra(j, src, pc, 16, &count);
    if (!entry) {
        pd_jit_reset(j);
        entry = pd_jit_translate_ra(j, src, pc, 16, &count);
        if (!entry) return NULL;
    }
    b = pd_jit_find(j, pc);
    if (!b) return NULL;

    uint32_t bytes = 0, cyc = 0;
    uint8_t last_op = 0;
    const uint16_t *s = src;
    for (int k = 0; k < count; k++) {
        uint8_t op = (uint8_t)(s[0] >> 10);
        last_op = op;
        cyc += opcycle[op];
        int hw = (op >= 0x28) ? 2 : 1;
        s += hw;
        bytes += (uint32_t)hw * 2;
    }
    b->pc_bytes = (uint16_t)bytes;
    b->cycles   = (uint16_t)cyc;
    b->last_op  = last_op;
    b->gen      = j->flush_gen;
    return b;
}
