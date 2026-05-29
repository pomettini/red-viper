// Thumb-2 instruction emitter for the Playdate V810 dynarec.
//
// Writes machine code into a halfword cursor (uint16_t*). Cortex-M7 executes
// Thumb-2 only. Encodings below are taken from the ARMv7-M Architecture
// Reference Manual; bit layouts are documented inline because a wrong bit is
// a hard fault, not a compile error. Every encoder is validated on-device by
// the self-test in pd_jit_test.c before being used by block translation.
//
// Register convention for the emitter: plain ARM register numbers 0..15.
// r13=sp, r14=lr, r15=pc. The 32-bit data-processing forms below accept
// r0..r12 freely.

#ifndef T2_EMIT_H
#define T2_EMIT_H

#include <stdint.h>

static inline void t2_hw(uint16_t **p, uint16_t h) { *(*p)++ = h; }
static inline void t2_w32(uint16_t **p, uint16_t hi, uint16_t lo) {
    (*p)[0] = hi; (*p)[1] = lo; *p += 2;
}

// ---------------------------------------------------------------------------
// 16-bit Thumb

// bx Rm        T1: 0100 0111 0 Rm(4) 000
static inline void t2_bx(uint16_t **p, int rm) { t2_hw(p, 0x4700 | (rm << 3)); }

// blx Rm       T1: 0100 0111 1 Rm(4) 000
static inline void t2_blx(uint16_t **p, int rm) { t2_hw(p, 0x4780 | (rm << 3)); }

// push list    T1: 1011 010 M rrrrrrrr   (bit8=M=lr, bits0-7=r0..r7)
static inline void t2_push(uint16_t **p, uint16_t list) { t2_hw(p, 0xB400 | list); }
// pop  list    T1: 1011 110 P rrrrrrrr   (bit8=P=pc,  bits0-7=r0..r7)
static inline void t2_pop(uint16_t **p, uint16_t list) { t2_hw(p, 0xBC00 | list); }

// mov Rd, Rm   T1 (any reg): 0100 0110 D Rm(4) Rd(3)
static inline void t2_mov_reg(uint16_t **p, int rd, int rm) {
    t2_hw(p, 0x4600 | ((rd & 8) << 4) | (rm << 3) | (rd & 7));
}

static inline void t2_nop(uint16_t **p) { t2_hw(p, 0xBF00); }

// ---------------------------------------------------------------------------
// 32-bit Thumb-2

// movw Rd,#imm16   T3: 11110 i 100100 imm4 | 0 imm3 Rd imm8
static inline void t2_movw(uint16_t **p, int rd, uint16_t imm) {
    uint16_t i    = (imm >> 11) & 1;
    uint16_t imm4 = (imm >> 12) & 0xF;
    uint16_t imm3 = (imm >> 8)  & 0x7;
    uint16_t imm8 =  imm        & 0xFF;
    t2_w32(p, 0xF240 | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8);
}

// movt Rd,#imm16   T1: 11110 i 101100 imm4 | 0 imm3 Rd imm8
static inline void t2_movt(uint16_t **p, int rd, uint16_t imm) {
    uint16_t i    = (imm >> 11) & 1;
    uint16_t imm4 = (imm >> 12) & 0xF;
    uint16_t imm3 = (imm >> 8)  & 0x7;
    uint16_t imm8 =  imm        & 0xFF;
    t2_w32(p, 0xF2C0 | (i << 10) | imm4, (imm3 << 12) | (rd << 8) | imm8);
}

// Load an arbitrary 32-bit constant into Rd (movw [+ movt]).
static inline void t2_mov32(uint16_t **p, int rd, uint32_t v) {
    t2_movw(p, rd, (uint16_t)(v & 0xFFFF));
    if (v >> 16) t2_movt(p, rd, (uint16_t)(v >> 16));
}

// Data-processing (register), T2/T3 forms, S=0:
//   hw1 = <op> Rn ,  hw2 = (Rd<<8) | Rm   (imm3=0, imm2=0, type=0 -> LSL #0)
static inline void t2_add_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEB00 | rn, (rd << 8) | rm); }
static inline void t2_sub_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEBA0 | rn, (rd << 8) | rm); }
static inline void t2_and_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEA00 | rn, (rd << 8) | rm); }
static inline void t2_orr_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEA40 | rn, (rd << 8) | rm); }
static inline void t2_eor_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEA80 | rn, (rd << 8) | rm); }

// cmp Rn, Rm   T2: 1110 1011 1011 Rn | 0 imm3 1111 imm2 type Rm  (Rd=PC=15, S=1)
static inline void t2_cmp_reg(uint16_t **p, int rn, int rm) { t2_w32(p, 0xEBB0 | rn, 0x0F00 | rm); }

// ldr Rt,[Rn,#imm12]   T3: 1111 1000 1101 Rn | Rt imm12
static inline void t2_ldr_imm(uint16_t **p, int rt, int rn, uint16_t imm12) {
    t2_w32(p, 0xF8D0 | rn, (rt << 12) | (imm12 & 0xFFF));
}
// str Rt,[Rn,#imm12]   T3: 1111 1000 1100 Rn | Rt imm12
static inline void t2_str_imm(uint16_t **p, int rt, int rn, uint16_t imm12) {
    t2_w32(p, 0xF8C0 | rn, (rt << 12) | (imm12 & 0xFFF));
}

#endif // T2_EMIT_H
