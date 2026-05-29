#include <string.h>
#include "pd_jit.h"
#include "t2_emit.h"

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
