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

// Flag-setting (S=1) variants of add/sub — set ARM N,Z,C,V.
static inline void t2_adds_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEB10 | rn, (rd << 8) | rm); }
static inline void t2_subs_reg(uint16_t **p, int rd, int rn, int rm) { t2_w32(p, 0xEBB0 | rn, (rd << 8) | rm); }

// mrs Rd, APSR   T1: 1111 0011 1110 1111 | 1000 Rd 0000 0000
static inline void t2_mrs_apsr(uint16_t **p, int rd) { t2_w32(p, 0xF3EF, 0x8000 | (rd << 8)); }

// ubfx Rd,Rn,#lsb,#width   T1: 1111 0011 1100 Rn | 0 imm3 Rd imm2 (0) widthm1
//   lsb = imm3:imm2
static inline void t2_ubfx(uint16_t **p, int rd, int rn, int lsb, int width) {
    uint16_t imm3 = (lsb >> 2) & 0x7, imm2 = lsb & 0x3;
    t2_w32(p, 0xF3C0 | rn, (imm3 << 12) | (rd << 8) | (imm2 << 6) | ((width - 1) & 0x1F));
}

// eor Rd,Rn,#imm8 (modified immediate, simple imm8 path), T1: op=0100
static inline void t2_eor_imm(uint16_t **p, int rd, int rn, uint8_t imm8) {
    t2_w32(p, 0xF080 | rn, (rd << 8) | imm8);
}

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

// clz Rd,Rm   T1: 1111 1010 1011 Rm | 1111 Rd 1000 Rm   (Rm appears twice)
static inline void t2_clz(uint16_t **p, int rd, int rm) {
    t2_w32(p, 0xFAB0 | rm, 0xF080 | (rd << 8) | rm);
}

// MOV (shifted register) immediate amount, T2:
//   hw1 = 1110 1010 0100 1111 (=0xEA4F, S=0, Rn=PC)
//   hw2 = 0 imm3 Rd imm2 type Rm,  shift = imm3:imm2,  type: 00=LSL 01=LSR 10=ASR
static inline void t2_shift_imm(uint16_t **p, int rd, int rm, int type, int sh) {
    uint16_t imm3 = (sh >> 2) & 0x7, imm2 = sh & 0x3;
    t2_w32(p, 0xEA4F, (imm3 << 12) | (rd << 8) | (imm2 << 6) | ((type & 3) << 4) | rm);
}
static inline void t2_lsl_imm(uint16_t **p, int rd, int rm, int sh) { t2_shift_imm(p, rd, rm, 0, sh); }
static inline void t2_lsr_imm(uint16_t **p, int rd, int rm, int sh) { t2_shift_imm(p, rd, rm, 1, sh); }
static inline void t2_asr_imm(uint16_t **p, int rd, int rm, int sh) { t2_shift_imm(p, rd, rm, 2, sh); }

// Data-processing (modified immediate), S=0. Only the simple imm8 path
// (i=0, imm3=0, value 0..255) is exposed — enough for small masks.
//   hw1 = 1111 0 i 0 <op4> S nnnn ,  hw2 = 0 imm3 Rd imm8
static inline void t2_bic_imm(uint16_t **p, int rd, int rn, uint8_t imm8) {
    t2_w32(p, 0xF020 | rn, (rd << 8) | imm8); // op=0001 (BIC)
}
static inline void t2_orr_imm(uint16_t **p, int rd, int rn, uint8_t imm8) {
    t2_w32(p, 0xF040 | rn, (rd << 8) | imm8); // op=0010 (ORR)
}

#endif // T2_EMIT_H
