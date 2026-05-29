#include <math.h>
#include "v810_cpu.h"
#include "v810_mem.h"
#include "v810_opt.h"
#include "vb_types.h"
#include "drc_core.h"

// On Playdate the data accessors are routed through function pointers
// (g_rv_*) so they can be relocated into DTCM each frame (see pd_itcm.c).
// Everywhere else they call the plain mem_* functions directly.
#if defined(TARGET_PLAYDATE) || defined(TARGET_SIMULATOR)
#include "pd_itcm.h"
// Debug hook: logs each loop the busywait detector classifies as idle, once
// per unique branch PC. Implemented in pd_core.c. Used to find loops that get
// wrongly fast-forwarded (game-compat debugging).
void rv_busywait_dbg(WORD branch_pc, WORD target_pc);
#define RV_BW_DBG(b, t) rv_busywait_dbg((b), (t))
#else
#define MEM_RBYTE   mem_rbyte
#define MEM_RHWORD  mem_rhword
#define MEM_RWORD   mem_rword
#define MEM_WBYTE   mem_wbyte
#define MEM_WHWORD  mem_whword
#define MEM_WWORD   mem_wword
#define RV_BW_DBG(b, t) ((void)0)
#endif

// Fast-path instruction fetch. The generic MEM_RHWORD() lives in another
// translation unit (no cross-TU inlining without LTO), returns a uint64_t
// with wait-state bits packed in the high word, and dispatches through a
// region switch. For instruction fetch we only need the 16-bit value, and
// the PC is overwhelmingly in ROM, so read straight from the ROM buffer and
// fall back to mem_rhword for the rare non-ROM fetch (e.g. WRAM-resident
// code). Behaviour is identical: the interpreter derives timing from the
// opcycle[] table, not the discarded wait bits.
//
// On the 3DS+DRC build the interpreter never runs with PC in ROM (the DRC
// owns that path and the interpreter loop exits when PC enters ROM), so this
// fast path is inert there and only benefits the DRC-less Playdate build.
static inline HWORD itrp_fetch(WORD PC) {
    if ((PC & 0x07000000) == 0x07000000)
        return *(HWORD *)(V810_ROM1.off + (PC & (0x07000000 | (MAX_ROM_SIZE - 1)) & ~1));
    return (HWORD)MEM_RHWORD(PC);
}

static bool get_cond(BYTE code, WORD psw) {
    bool cond = false;
    switch (0x40 | (code & ~8)) {
        case V810_OP_BV: cond = psw & 4; break;
        case V810_OP_BL: cond = psw & 8; break;
        case V810_OP_BE: cond = psw & 1; break;
        case V810_OP_BNH: cond = psw & 9; break;
        case V810_OP_BN: cond = psw & 2; break;
        case V810_OP_BR: cond = true; break;
        case V810_OP_BLT: cond = !!(psw & 4) != !!(psw & 2); break;
        case V810_OP_BLE: cond = (psw & 1) || !!(psw & 4) != !!(psw & 2); break;
    }
    if (code & 8) cond = !cond;
    return cond;
}

// --- Busywait detection -----------------------------------------------------
//
// The DRC (source/arm/drc_core.c) gets its speed partly by recognising idle
// spin loops and fast-forwarding to the next scheduled event instead of
// executing the loop body millions of times. The DRC-less Playdate build
// needs the same trick in the interpreter, or idle screens (and the idle
// time within real gameplay) cost a full frame of brute-forced iterations.
//
// A backward branch whose loop body is *idempotent* — re-running it produces
// identical register/memory state — cannot change its own exit condition
// until an interrupt fires, so we can jump straight to the next event. The
// whitelist below mirrors the DRC's drc_findLastConditionalInst(): loads,
// moves, compares, `or rX,rX`, `add r0,rX`. Crucially it EXCLUDES counters
// (ADD_I / SUB / etc.) and pointer-chasing loads (reg1==reg2), because
// skipping those would corrupt state. Anything not whitelisted -> not a
// busywait (conservative; we just don't optimise it).
//
// Verdicts are cached per branch PC. Only ROM branches are classified: ROM
// can't self-modify, so a verdict stays valid for the life of the ROM.

static bool busywait_body_ok(WORD target_pc, WORD branch_pc) {
    WORD pc = target_pc;
    while (pc < branch_pc) {
        HWORD instr = itrp_fetch(pc);
        BYTE opcode = instr >> 10;
        BYTE reg1 = instr & 31;
        BYTE reg2 = (instr >> 5) & 31;
        pc += (opcode >= 0x28) ? 4 : 2;
        switch (opcode) {
            case V810_OP_LD_B: case V810_OP_LD_H: case V810_OP_LD_W:
            case V810_OP_IN_B: case V810_OP_IN_H: case V810_OP_IN_W:
                // a load into its own address register is a pointer chase,
                // not idempotent
                if (reg1 == reg2) return false;
                break;
            case V810_OP_MOV:  case V810_OP_MOV_I:
            case V810_OP_MOVEA: case V810_OP_MOVHI:
            case V810_OP_AND:  case V810_OP_ANDI:
            case V810_OP_CMP:  case V810_OP_CMP_I:
                break;
            case V810_OP_OR:
                if (reg1 != reg2) return false; // only `or rX,rX` is a no-op
                break;
            case V810_OP_ADD:
                if (reg1 != 0) return false;    // only `add r0,rX` is a no-op
                break;
            default:
                return false;
        }
    }
    return true;
}

#define BW_CACHE_SIZE 256
static struct { WORD branch_pc; bool busy; } bw_cache[BW_CACHE_SIZE];

static inline bool is_busywait_loop(WORD target_pc, WORD branch_pc) {
    unsigned idx = (branch_pc >> 1) & (BW_CACHE_SIZE - 1);
    if (bw_cache[idx].branch_pc == branch_pc) return bw_cache[idx].busy;
    bool b = busywait_body_ok(target_pc, branch_pc);
    bw_cache[idx].branch_pc = branch_pc;
    bw_cache[idx].busy = b;
    if (b) RV_BW_DBG(branch_pc, target_pc);
    return b;
}

int interpreter_run(void) {
    // keep PC and cycles in local variables for extra speed
    // can't do this with PSW because interrupts modify it
    WORD PC = vb_state->v810_state.PC;
    WORD last_PC = PC;
    WORD cycles = vb_state->v810_state.cycles;
    BYTE last_opcode = 0;
    WORD target = cycles;
    do {
        if ((SWORD)(target - cycles) <= 0) {
            vb_state->v810_state.PC = PC;
            if (serviceInt(cycles, PC) && (PC != vb_state->v810_state.PC || vb_state->v810_state.ret)) {
                // interrupt triggered, so we exit
                // PC may have been modified so don't reset it
                vb_state->v810_state.cycles = cycles;
                return 0;
            }
            target = cycles + vb_state->v810_state.cycles_until_event_partial;
        }
        HWORD instr = itrp_fetch(PC);
        PC += 2;
        BYTE opcode = instr >> 10;
        BYTE reg1 = instr & 31;
        BYTE reg2 = (instr >> 5) & 31;
        cycles += opcycle[opcode];
        if (opcode < 0x20) {
            // small instr
            WORD reg1_val = 0;
            if (!(opcode & 0x10) && reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
            switch (opcode) {
                case V810_OP_MOV:
                    vb_state->v810_state.P_REG[reg2] = reg1_val;
                    break;
                case V810_OP_ADD: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val + reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SUB: case V810_OP_CMP: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val - reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ reg1_val) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHL: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val << reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (32 - reg1_val)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHR: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_JMP:
                    PC = reg1_val;
                    break;
                case V810_OP_SAR: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    reg1_val &= 31;
                    WORD res = (SWORD)reg2_val >> reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1_val != 0 ? (reg2_val >> (reg1_val - 1)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MUL: {
                    SWORD reg2_val = reg2 ? (SWORD)vb_state->v810_state.P_REG[reg2] : 0;
                    int64_t res = (int64_t)(SWORD)reg1_val * (int64_t)reg2_val;
                    bool ov = res != (int64_t)(int32_t)res;
                    bool z = res == 0;
                    bool s = res < 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2);
                    vb_state->v810_state.P_REG[30] = (WORD)(res >> 32);
                    vb_state->v810_state.P_REG[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIV: {
                    SWORD reg2_val = reg2 ? (SWORD)vb_state->v810_state.P_REG[reg2] : 0;
                    if (reg2_val == 0x80000000 && (SWORD)reg1_val == -1) {
                        vb_state->v810_state.P_REG[30] = 0;
                        vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | 6;
                    } else {
                        vb_state->v810_state.P_REG[30] = reg2_val % (SWORD)reg1_val;
                        SWORD res = reg2_val / (SWORD)reg1_val;
                        bool z = res == 0;
                        bool s = res < 0;
                        vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | z | (s << 1);
                        vb_state->v810_state.P_REG[reg2] = res;
                    }
                    break;
                }
                case V810_OP_MULU: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    uint64_t res = (uint64_t)reg1_val * (uint64_t)reg2_val;
                    bool ov = res != (uint64_t)(uint32_t)res;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2);
                    vb_state->v810_state.P_REG[30] = (WORD)(res >> 32);
                    vb_state->v810_state.P_REG[reg2] = (WORD)res;
                    break;
                }
                case V810_OP_DIVU: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    vb_state->v810_state.P_REG[30] = reg2_val % reg1_val;
                    WORD res = reg2_val / reg1_val;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | z | (s << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_OR: {
                    WORD res = (reg2 ? vb_state->v810_state.P_REG[reg2] : 0) | reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_AND: {
                    WORD res = (reg2 ? vb_state->v810_state.P_REG[reg2] : 0) & reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_XOR: {
                    WORD res = (reg2 ? vb_state->v810_state.P_REG[reg2] : 0) ^ reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_NOT: {
                    WORD res = ~reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MOV_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    vb_state->v810_state.P_REG[reg2] = imm;
                    break;
                }
                case V810_OP_ADD_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg2_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SETF: {
                    vb_state->v810_state.P_REG[reg2] = get_cond(reg1, vb_state->v810_state.S_REG[PSW]);
                    break;
                }
                case V810_OP_CMP_I: {
                    WORD imm = reg1 & 0x10 ? reg1 | 0xfffffff0 : reg1;
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val - imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)((reg2_val ^ imm) & (reg2_val ^ res)) < 0;
                    bool cy = (unsigned)reg2_val < (unsigned)imm;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    if (opcode == V810_OP_SUB) vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHL_I: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val << reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (32 - reg1)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_SHR_I: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_CLI:
                    vb_state->v810_state.S_REG[PSW] &= ~(1 << 12);
                    break;
                case V810_OP_SAR_I: {
                    WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                    WORD res = (SWORD)reg2_val >> reg1;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = false;
                    bool cy = reg1 != 0 ? (reg2_val >> (reg1 - 1)) & 1 : 0;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                // case V810_OP_TRAP:
                case V810_OP_RETI:
                    if (vb_state->v810_state.S_REG[PSW] & PSW_NP) {
                        PC = vb_state->v810_state.S_REG[FEPC];
                        vb_state->v810_state.S_REG[PSW] = vb_state->v810_state.S_REG[FEPSW];
                    } else {
                        PC = vb_state->v810_state.S_REG[EIPC];
                        vb_state->v810_state.S_REG[PSW] = vb_state->v810_state.S_REG[EIPSW];
                    }
                    break;
                case V810_OP_HALT: {
                    cycles = target;
                    vb_state->v810_state.PC = PC;
                    do {
                        cycles += vb_state->v810_state.cycles_until_event_partial;
                        vb_state->v810_state.cycles_until_event_partial = vb_state->v810_state.cycles_until_event_full = 0;
                        vb_state->v810_state.cycles = cycles;
                        serviceInt(cycles, PC);
                    } while (!vb_state->v810_state.ret && vb_state->v810_state.PC == PC);
                    if (vb_state->v810_state.PC == PC) {
                        // no interrupt triggered, so repeat the halt
                        vb_state->v810_state.PC = last_PC;
                    }
                    // PC was modified so don't reset it
                    return 0;
                }
                case V810_OP_LDSR:
                    vb_state->v810_state.S_REG[reg1] = (reg2 ? vb_state->v810_state.P_REG[reg2] : 0);
                    break;
                case V810_OP_STSR:
                    vb_state->v810_state.P_REG[reg2] = vb_state->v810_state.S_REG[reg1];
                    break;
                case V810_OP_SEI:
                    vb_state->v810_state.S_REG[PSW] |= 1 << 12;
                    break;
                case V810_OP_BSTR: {
                    typedef bool (*bstr_func)(WORD,WORD,WORD,WORD);
                    bstr_func func = (bstr_func)bssuboptable[reg1].func;
                    WORD lastarg = reg1 < 4 ? vb_state->v810_state.P_REG[27] & 31 : ((vb_state->v810_state.P_REG[27] & 31)) | ((vb_state->v810_state.P_REG[26] & 31) << 5) | ((target - cycles) << 10);
                    WORD res = func(vb_state->v810_state.P_REG[30], vb_state->v810_state.P_REG[29], vb_state->v810_state.P_REG[28], lastarg);
                    if (reg1 < 4) {
                        vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~1) | !res;
                    } else {
                        vb_state->v810_state.cycles += res;
                        if (vb_state->v810_state.P_REG[28]) {
                            PC = last_PC;
                        }
                    }
                    break;
                }
                default: {
                    vb_state->v810_state.PC = last_PC;
                    return DRC_ERR_BAD_INST;
                }
            }
        } else if (opcode < 0x28) {
            // branch
            if (get_cond(instr >> 9, vb_state->v810_state.S_REG[PSW])) {
                SHWORD disp = instr & (1 << 8) ? (instr | 0xfe00) : (instr & ~0xfe00);
                PC += disp - 2;
                // Busywait: a taken backward branch in ROM whose loop body is
                // idempotent can't change its own exit condition until an
                // interrupt fires. Fast-forward to the next event instead of
                // spinning (mirrors the DRC and the HALT handler below).
                // last_PC holds the branch instruction's own address; PC now
                // holds the branch target.
                // (Gate RV_NO_BUSYWAIT lets us A/B the detector for game-compat
                // debugging without removing it.)
#ifndef RV_NO_BUSYWAIT
                if (disp <= 0
                    && (last_PC & 0x07000000) == 0x07000000
                    && is_busywait_loop(PC, last_PC)) {
                    // Idle spin: nothing the loop does changes its own exit
                    // condition until the next scheduled event, so skip the
                    // redundant iterations by jumping cycles to that event,
                    // then RETURN so the loop body re-runs and re-evaluates
                    // its condition. We must NOT keep fast-forwarding past
                    // events HALT-style: the exit may be a non-interrupt
                    // state change (e.g. polling DPSTTS & DPBSY), which only
                    // gets re-checked by re-executing the loop body. (That
                    // HALT-style bug hung Mario's Tennis's boot poll forever.)
                    if ((SWORD)(target - cycles) > 0)
                        cycles = target;
                    vb_state->v810_state.PC = PC;
                    vb_state->v810_state.cycles = cycles;
                    return 0;
                }
#endif
            } else {
                // branch not taken, so it only took 1 cycle
                cycles -= 2;
            }
        } else {
            // long instr
            HWORD instr2 = itrp_fetch(PC);
            PC += 2;
            switch (opcode) {
                case V810_OP_MOVEA: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = reg1_val + (SHWORD)instr2;
                    break;
                }
                case V810_OP_ADDI: {
                    WORD reg1_val = reg1 ? vb_state->v810_state.P_REG[reg1] : 0;
                    WORD imm = (SHWORD)instr2;
                    WORD res = reg1_val + imm;
                    bool z = res == 0;
                    bool s = (SWORD)res < 0;
                    bool ov = (SWORD)(~(reg1_val ^ imm) & (reg1_val ^ res)) < 0;
                    bool cy = (unsigned)res < (unsigned)reg1_val;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | (s << 1) | (ov << 2) | (cy << 3);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_JAL:
                    vb_state->v810_state.P_REG[31] = PC;
                    // fallthrough
                case V810_OP_JR: {
                    SWORD disp = instr2 | ((SWORD)instr << 16);
                    if (disp & 0x02000000) disp |= 0xfc000000;
                    else disp &= ~(0xfc000000);
                    PC += disp - 4;
                    break;
                }
                case V810_OP_ORI: {
                    WORD res = instr2;
                    if (reg1) res |= vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_ANDI: {
                    WORD res = 0;
                    if (reg1) res = vb_state->v810_state.P_REG[reg1] & instr2;
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_XORI: {
                    WORD res = instr2;
                    if (reg1) res ^= vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0x7) | (res == 0) | (((SWORD)res < 0) << 1);
                    vb_state->v810_state.P_REG[reg2] = res;
                    break;
                }
                case V810_OP_MOVHI: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = reg1_val + ((WORD)instr2 << 16);
                    break;
                }
                case V810_OP_LD_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = (SBYTE)MEM_RBYTE(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = (SHWORD)MEM_RHWORD(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_LD_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = MEM_RWORD(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 4 cycles instead of 5
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_IN_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = (BYTE)MEM_RBYTE(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = (HWORD)MEM_RHWORD(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 2 cycles instead of 3
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 2;
                    }
                    break;
                }
                case V810_OP_IN_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    vb_state->v810_state.P_REG[reg2] = (WORD)MEM_RWORD(reg1_val + (SHWORD)instr2);
                    if ((last_opcode & 0x34) == 0x30 && (last_opcode & 3) != 2) {
                    // load immediately following another load takes 4 cycles instead of 5
                        cycles -= 1;
                    } else if (opcycle[last_opcode] > 4) {
                        // load following instruction taking "many" cycles only takes 1 cycles
                        // guessing "many" is 4 for now
                        cycles -= 4;
                    }
                    break;
                }
                case V810_OP_ST_B: case V810_OP_OUT_B: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    BYTE reg2_val = 0;
                    if (reg2) reg2_val = vb_state->v810_state.P_REG[reg2];
                    MEM_WBYTE(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 2 cycles instead of 1
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_H: case V810_OP_OUT_H: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    HWORD reg2_val = 0;
                    if (reg2) reg2_val = vb_state->v810_state.P_REG[reg2];
                    MEM_WHWORD(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 2 cycles instead of 1
                        cycles += 1;
                    }
                    break;
                }
                case V810_OP_ST_W: case V810_OP_OUT_W: {
                    WORD reg1_val = 0;
                    if (reg1) reg1_val = vb_state->v810_state.P_REG[reg1];
                    WORD reg2_val = 0;
                    if (reg2) reg2_val = vb_state->v810_state.P_REG[reg2];
                    MEM_WWORD(reg1_val + (SHWORD)instr2, reg2_val);
                    if ((last_opcode & 0x34) == 0x34 && (last_opcode & 3) != 2) {
                        // with two consecutive stores, the second takes 4 cycles instead of 1
                        cycles += 3;
                    }
                    break;
                }
                // case V810_OP_CAXI:
                case V810_OP_FPP: {
                    int subop = instr2 >> 10;
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wstrict-aliasing"
                    if (subop == V810_OP_CVT_WS) {
                        float res = reg1 ? (float)(SWORD)vb_state->v810_state.P_REG[reg1] : 0;
                        bool z = res == 0;
                        int scy = res < 0 ? 0xa : 0;
                        vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | scy;
                        *(float*)&vb_state->v810_state.P_REG[reg2] = res;
                    } else if (!(subop & 8) || subop == V810_OP_TRNC_SW) {
                        // float
                        float reg1_val = reg1 ? *(float*)&vb_state->v810_state.P_REG[reg1] : 0;
                        if (subop == V810_OP_CVT_SW) {
                            SWORD res = round(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | scy;
                            vb_state->v810_state.P_REG[reg2] = res;
                        } else if (subop == V810_OP_TRNC_SW) {
                            SWORD res = (SWORD)(reg1_val);
                            bool z = res == 0;
                            int scy = res < 0 ? 2 : 0;
                            vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | scy;
                            vb_state->v810_state.P_REG[reg2] = res;
                        } else {
                            float reg2_val = reg2 ? *(float*)&vb_state->v810_state.P_REG[reg2] : 0;
                            float res;
                            switch (subop) {
                                case V810_OP_ADDF_S:
                                    res = reg2_val + reg1_val;
                                    break;
                                case V810_OP_CMPF_S:
                                case V810_OP_SUBF_S:
                                    res = reg2_val - reg1_val;
                                    break;
                                case V810_OP_MULF_S:
                                    res = reg2_val * reg1_val;
                                    break;
                                case V810_OP_DIVF_S:
                                    res = reg2_val / reg1_val;
                                    break;
                                default:
                                    return DRC_ERR_BAD_INST;
                            }
                            bool z = res == 0;
                            int scy = res < 0 ? 0xa : 0;
                            vb_state->v810_state.S_REG[PSW] = (vb_state->v810_state.S_REG[PSW] & ~0xf) | z | scy;
                            if (subop != V810_OP_CMPF_S) *(float*)&vb_state->v810_state.P_REG[reg2] = res;
                        }
                    } else {
                        // extended
                        WORD reg2_val = reg2 ? vb_state->v810_state.P_REG[reg2] : 0;
                        switch (subop) {
                            case V810_OP_MPYHW:
                                vb_state->v810_state.P_REG[reg2] *= reg1 ? (int)(vb_state->v810_state.P_REG[reg1] << 15) >> 15 : 0;
                                break;
                            case V810_OP_REV:
                                vb_state->v810_state.P_REG[reg2] = reg1 ? ins_rev(vb_state->v810_state.P_REG[reg1]) : 0;
                                break;
                            case V810_OP_XB:
                                vb_state->v810_state.P_REG[reg2] = (reg2_val & 0xFFFF0000) | ((reg2_val << 8) & 0xFF00) | ((reg2_val >> 8) & 0xFF);
                                break;
                            case V810_OP_XH:
                                vb_state->v810_state.P_REG[reg2] = (reg2_val << 16) | (reg2_val >> 16);
                                break;
                            default:
                                return DRC_ERR_BAD_INST;
                        }
                    }
                    #pragma GCC diagnostic pop
                    break;
                }
                default: {
                    vb_state->v810_state.PC = last_PC;
                    return DRC_ERR_BAD_INST;
                }
            }
        }
        last_opcode = opcode;
        if ((PC & 0x07000000) < 0x05000000) {
            vb_state->v810_state.PC = last_PC;
            return DRC_ERR_BAD_PC;
        }
        last_PC = PC;
    } while (!vb_state->v810_state.ret && (!DRC_AVAILABLE || (PC & 0x07000000) != 0x07000000));
    vb_state->v810_state.PC = PC;
    vb_state->v810_state.cycles = cycles;
    return 0;
}