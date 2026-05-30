// Dynarec feasibility + Thumb-2 emitter self-test.
//
// Step 1 (RWX probe) confirmed: generated Thumb-2 executes from a heap
// (SDRAM) buffer, and pd->system->clearICache() is the privilege-safe flush
// (the SCB cache registers bus-fault from unprivileged app code).
//
// Step 2 validates the t2_emit.h encoder set on-device before block
// translation depends on it. Each sub-test emits a small function, flushes
// the I-cache, calls it, and checks the result. A wrong encoding is a hard
// fault, so getting a clean pass here de-risks the whole emitter.
//
// DEVICE ONLY: executes generated machine code, which would crash the Mac
// simulator. No-op (logs skipped) on the simulator.

#include <stdint.h>
#include "pd_api.h"
#include "pd_jit_test.h"

#if defined(TARGET_PLAYDATE) && !defined(TARGET_SIMULATOR)

#include "t2_emit.h"
#include "pd_jit.h"
#include "pd_itcm.h"   // g_rv_* accessors, for the load/store round-trip test

typedef int  (*fn_i_i)(int);
typedef void (*fn_v_p)(int *);
typedef int  (*fn_i_v)(void);

void pd_jit_test(PlaydateAPI *pd) {
    // One shared executable code buffer in heap/SDRAM (proven executable).
    uint16_t *buf = (uint16_t *)pd->system->realloc(NULL, 256);
    if (!buf) { pd->system->logToConsole("pd_jit_test: alloc failed"); return; }

    int pass = 0, total = 0;

    // --- test 1: movw + add_reg + bx  ->  f(x) = x + 100 ---
    {
        uint16_t *p = buf;
        t2_movw(&p, 1, 100);     // movw r1, #100
        t2_add_reg(&p, 0, 0, 1); // add  r0, r0, r1
        t2_bx(&p, 14);           // bx   lr
        pd->system->clearICache();
        fn_i_i f = (fn_i_i)((uintptr_t)buf | 1u);
        int r = f(5);
        total++; if (r == 105) pass++;
        pd->system->logToConsole("  t1 movw/add/bx: f(5)=%d exp 105 %s", r, r == 105 ? "ok" : "BAD");
    }

    // --- test 2: mov32 (movw+movt) + add  ->  f(x) = x + 0x00123456 ---
    {
        uint16_t *p = buf;
        t2_mov32(&p, 1, 0x00123456u); // movw/movt r1, #0x123456
        t2_add_reg(&p, 0, 0, 1);
        t2_bx(&p, 14);
        pd->system->clearICache();
        fn_i_i f = (fn_i_i)((uintptr_t)buf | 1u);
        int r = f(0);
        total++; if (r == 0x123456) pass++;
        pd->system->logToConsole("  t2 mov32/add: f(0)=0x%x exp 0x123456 %s", r, r == 0x123456 ? "ok" : "BAD");
    }

    // --- test 3: ldr/str  ->  g(p): p[1] = p[0] - p[2] ---
    {
        uint16_t *p = buf;
        t2_ldr_imm(&p, 1, 0, 0);  // ldr r1, [r0, #0]
        t2_ldr_imm(&p, 2, 0, 8);  // ldr r2, [r0, #8]
        t2_sub_reg(&p, 1, 1, 2);  // sub r1, r1, r2
        t2_str_imm(&p, 1, 0, 4);  // str r1, [r0, #4]
        t2_bx(&p, 14);
        pd->system->clearICache();
        fn_v_p g = (fn_v_p)((uintptr_t)buf | 1u);
        int arr[3] = { 50, 0, 8 };
        g(arr);
        total++; if (arr[1] == 42) pass++;
        pd->system->logToConsole("  t3 ldr/sub/str: arr[1]=%d exp 42 %s", arr[1], arr[1] == 42 ? "ok" : "BAD");
    }

    pd->system->logToConsole("pd_jit_test: emitter self-test %d/%d passed -> %s",
                             pass, total, pass == total ? "EMITTER OK" : "EMITTER FAIL");

    pd->system->realloc(buf, 0);

    // --- test 4: code cache + block map (emit two blocks, look up, run) ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init failed");
            return;
        }
        int cpass = 0, ctotal = 0;

        uint32_t pcA = 0x07001000, pcB = 0x07002000;
        uint16_t *p;

        p = pd_jit_begin_block(&j, pcA);          // block A: return 1234
        t2_movw(&p, 0, 1234);
        t2_bx(&p, 14);
        j.code_pos = p;

        p = pd_jit_begin_block(&j, pcB);          // block B: return 5678
        t2_movw(&p, 0, 5678);
        t2_bx(&p, 14);
        j.code_pos = p;

        pd_jit_flush(&j);

        void *ea = pd_jit_lookup(&j, pcA);
        void *eb = pd_jit_lookup(&j, pcB);
        void *en = pd_jit_lookup(&j, 0x07009999); // not translated

        ctotal++; if (en == NULL) cpass++;
        if (ea) { int r = ((fn_i_v)ea)(); ctotal++; if (r == 1234) cpass++;
                  pd->system->logToConsole("  t4a blockA()=%d exp 1234 %s", r, r==1234?"ok":"BAD"); }
        if (eb) { int r = ((fn_i_v)eb)(); ctotal++; if (r == 5678) cpass++;
                  pd->system->logToConsole("  t4b blockB()=%d exp 5678 %s", r, r==5678?"ok":"BAD"); }

        pd->system->logToConsole("pd_jit_test: code-cache self-test %d/%d -> %s",
                                 cpass, ctotal, cpass == ctotal ? "CACHE OK" : "CACHE FAIL");
        pd_jit_free(&j);
    }

    // --- test 5: move-subset translator against a mock register file ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (xlate) failed");
            return;
        }

        // A little V810 move program (native halfword order):
        //   MOV_I r5, #7          ; r5 = 7
        //   MOV   r6, r5          ; r6 = 7
        //   MOVEA r7, r6, #100    ; r7 = 107
        //   MOVHI r8, r0, #0x1234 ; r8 = 0x12340000
        //   ADD   ...             ; unsupported -> ends block
        static const uint16_t prog[] = {
            0x40A7,           // MOV_I r5,#7   : (0x10<<10)|(5<<5)|7
            0x00C5,           // MOV   r6,r5   : (0x00<<10)|(6<<5)|5
            0xA0E6, 0x0064,   // MOVEA r7,r6,#100
            0xBD00, 0x1234,   // MOVHI r8,r0,#0x1234
            0x1800,           // JMP (unsupported now that ADD is translated) -> stop
        };

        int count = 0;
        void *entry = pd_jit_translate(&j, prog, 0x07000000, 16, &count);
        pd_jit_flush(&j);

        int xpass = 0, xtotal = 0;
        xtotal++; if (count == 4) xpass++;
        pd->system->logToConsole("  t5 translated %d insns (exp 4) %s", count, count == 4 ? "ok" : "BAD");

        if (entry) {
            uint32_t reg[32];
            memset(reg, 0, sizeof(reg));
            ((void (*)(uint32_t *))entry)(reg);
            struct { int r; uint32_t exp; const char *nm; } chk[] = {
                {5, 7,          "r5"},
                {6, 7,          "r6"},
                {7, 107,        "r7"},
                {8, 0x12340000, "r8"},
            };
            for (int k = 0; k < 4; k++) {
                xtotal++;
                int good = reg[chk[k].r] == chk[k].exp;
                if (good) xpass++;
                pd->system->logToConsole("  t5 %s=0x%x exp 0x%x %s",
                                         chk[k].nm, (unsigned)reg[chk[k].r],
                                         (unsigned)chk[k].exp, good ? "ok" : "BAD");
            }
        }

        pd->system->logToConsole("pd_jit_test: translator self-test %d/%d -> %s",
                                 xpass, xtotal, xpass == xtotal ? "XLATE OK" : "XLATE FAIL");
        pd_jit_free(&j);
    }

    // --- test 6: logic ops (OR/AND/XOR) with Z/S flags ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (logic) failed");
            return;
        }
        //   MOVEA r5,r0,#0x1200   ; r5=0x1200
        //   MOVEA r6,r0,#0x0034   ; r6=0x34
        //   OR    r5,r6           ; r5=0x1234
        //   XOR   r6,r6           ; r6=0  (Z=1)
        //   AND   r5,r6           ; r5=0
        //   MOVHI r7,r0,#0x8000   ; r7=0x80000000
        //   OR    r7,r0           ; r7=0x80000000 (S=1) -> final PSW low3 = 2
        //   ADD   (unsupported)   ; stop
        static const uint16_t prog[] = {
            0xA0A0, 0x1200,
            0xA0C0, 0x0034,
            0x30A6,
            0x38C6,
            0x34A6,
            0xBCE0, 0x8000,
            0x30E0,
            0x1800,  // JMP (unsupported) -> stop
        };
        int count = 0;
        void *entry = pd_jit_translate(&j, prog, 0x07000000, 32, &count);
        pd_jit_flush(&j);

        int lpass = 0, ltotal = 0;
        ltotal++; if (count == 7) lpass++;
        pd->system->logToConsole("  t6 translated %d insns (exp 7) %s", count, count == 7 ? "ok" : "BAD");

        if (entry) {
            uint32_t reg[64];
            memset(reg, 0, sizeof(reg));
            reg[37] = 0xFF; // PSW (S_REG[5] @ byte 148) preset, to check upper bits survive
            ((void (*)(uint32_t *))entry)(reg);
            struct { int idx; uint32_t exp; const char *nm; } chk[] = {
                {5,  0,          "r5"},
                {6,  0,          "r6"},
                {7,  0x80000000, "r7"},
                {37, 0xFA,       "PSW"},  // (0xFF & ~7) | S(=2)
            };
            for (int k = 0; k < 4; k++) {
                ltotal++;
                int good = reg[chk[k].idx] == chk[k].exp;
                if (good) lpass++;
                pd->system->logToConsole("  t6 %s=0x%x exp 0x%x %s",
                                         chk[k].nm, (unsigned)reg[chk[k].idx],
                                         (unsigned)chk[k].exp, good ? "ok" : "BAD");
            }
        }

        pd->system->logToConsole("pd_jit_test: logic self-test %d/%d -> %s",
                                 lpass, ltotal, lpass == ltotal ? "LOGIC OK" : "LOGIC FAIL");
        pd_jit_free(&j);
    }

    // --- test 8: arithmetic (ADD/SUB) with Z/S/OV/CY flags ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (arith) failed");
            return;
        }
        //   MOV_I r5,#-1   ; 0xFFFFFFFF
        //   MOV_I r6,#1
        //   ADD   r5,r6    ; r5 = 0           (carry+zero)
        //   MOV_I r7,#0
        //   MOV_I r8,#1
        //   SUB   r7,r8    ; r7 = 0xFFFFFFFF  (borrow: S=1,CY=1) -> PSW low4 = 0xA
        //   ADD   (unsupported) ; stop
        static const uint16_t prog[] = {
            0x40BF, 0x40C1, 0x04A6,
            0x40E0, 0x4101, 0x08E8,
            0x1800,  // JMP (unsupported) -> stop
        };
        int count = 0;
        void *entry = pd_jit_translate(&j, prog, 0x07000000, 32, &count);
        pd_jit_flush(&j);

        int apass = 0, atotal = 0;
        atotal++; if (count == 6) apass++;
        pd->system->logToConsole("  t8 translated %d insns (exp 6) %s", count, count == 6 ? "ok" : "BAD");

        if (entry) {
            uint32_t reg[64];
            memset(reg, 0, sizeof(reg));
            reg[37] = 0xF0; // PSW preset; check upper bits survive
            ((void (*)(uint32_t *))entry)(reg);
            struct { int idx; uint32_t exp; const char *nm; } chk[] = {
                {5,  0x00000000, "r5(add)"},
                {7,  0xFFFFFFFF, "r7(sub)"},
                {37, 0xFA,       "PSW(S|CY)"},  // (0xF0 & ~0xf) | 0xA
            };
            for (int k = 0; k < 3; k++) {
                atotal++;
                int good = reg[chk[k].idx] == chk[k].exp;
                if (good) apass++;
                pd->system->logToConsole("  t8 %s=0x%x exp 0x%x %s",
                                         chk[k].nm, (unsigned)reg[chk[k].idx],
                                         (unsigned)chk[k].exp, good ? "ok" : "BAD");
            }
        }

        pd->system->logToConsole("pd_jit_test: arith self-test %d/%d -> %s",
                                 apass, atotal, apass == atotal ? "ARITH OK" : "ARITH FAIL");
        pd_jit_free(&j);
    }

    // --- test 9: register-allocating / lazy-flag translator (Stage 1) ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (RA) failed");
            return;
        }
        //   MOV_I r5,#-1       ; 0xFFFFFFFF
        //   MOV_I r6,#1
        //   ADD   r5,r6        ; r5 = 0            (intermediate flags, discarded)
        //   MOV   r7,r5        ; r7 = 0            (reg-reg move; reuse of r5)
        //   MOVEA r8,r6,#10    ; r8 = r6 + 10 = 11 (32-bit imm; reuse of r6)
        //   OR    r8,r5        ; r8 = 11 | 0 = 11  (logic flags, discarded)
        //   SUB   r9,r6        ; r9 = 0 - 1 = -1   (final flags: S=1,CY=1 -> 0xA)
        //   JMP               ; stop
        static const uint16_t prog[] = {
            0x40BF, 0x40C1, 0x04A6,
            0x00E5,
            0xA106, 0x000A,
            0x3105,
            0x0926,
            0x5000,  // unsupported op (shift) -> stop
        };
        int count = 0;
        void *entry = pd_jit_translate_ra(&j, prog, 0x07000000, 32, &count);
        pd_jit_flush(&j);

        int rpass = 0, rtotal = 0;
        rtotal++; if (count == 7) rpass++;
        pd->system->logToConsole("  t9 translated %d insns (exp 7) %s", count, count == 7 ? "ok" : "BAD");

        if (entry) {
            uint32_t reg[64];
            memset(reg, 0, sizeof(reg));
            reg[37] = 0xF0; // PSW preset; upper bits must survive
            ((void (*)(uint32_t *))entry)(reg);
            struct { int idx; uint32_t exp; const char *nm; } chk[] = {
                {5,  0x00000000, "r5"},
                {6,  0x00000001, "r6"},
                {7,  0x00000000, "r7"},
                {8,  0x0000000B, "r8"},
                {9,  0xFFFFFFFF, "r9"},
                {37, 0xFA,       "PSW(S|CY)"},  // (0xF0 & ~0xf) | 0xA
            };
            for (int k = 0; k < 6; k++) {
                rtotal++;
                int good = reg[chk[k].idx] == chk[k].exp;
                if (good) rpass++;
                pd->system->logToConsole("  t9 %s=0x%x exp 0x%x %s",
                                         chk[k].nm, (unsigned)reg[chk[k].idx],
                                         (unsigned)chk[k].exp, good ? "ok" : "BAD");
            }
        }

        pd->system->logToConsole("pd_jit_test: RA self-test %d/%d -> %s",
                                 rpass, rtotal, rpass == rtotal ? "RA OK" : "RA FAIL");
        pd_jit_free(&j);
    }

    // --- test 10: loads/stores via g_rv_* (Stage 2) ---
    // Round-trips values through real WRAM (0x05000000). Saves and restores the
    // two words we touch so the game still boots cleanly afterwards.
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 4096, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (LS) failed");
            return;
        }
        uint32_t save0 = (uint32_t)g_rv_rword(0x05000000);
        uint32_t save4 = (uint32_t)g_rv_rword(0x05000004);

        //   MOVHI r5,r0,#0x0500   ; r5 = 0x05000000 (WRAM)
        //   MOVEA r6,r0,#0x1234   ; r6 = 0x1234
        //   ST_W  [r5+0],r6       ; mem[..0] = 0x1234
        //   MOV_I r7,#0           ; clobber r7
        //   LD_W  r7,[r5+0]       ; r7 = 0x1234
        //   MOVEA r8,r0,#0x00FF   ; r8 = 0xFF
        //   ST_B  [r5+4],r8       ; mem[..4] = 0xFF
        //   LD_B  r9,[r5+4]       ; r9 = (SBYTE)0xFF = 0xFFFFFFFF
        //   IN_B  r10,[r5+4]      ; r10 = (BYTE)0xFF  = 0x000000FF
        //   JMP                   ; stop
        static const uint16_t prog[] = {
            0xBCA0, 0x0500,
            0xA0C0, 0x1234,
            0xDCC5, 0x0000,
            0x40E0,
            0xCCE5, 0x0000,
            0xA100, 0x00FF,
            0xD105, 0x0004,
            0xC125, 0x0004,
            0xE145, 0x0004,
            0x5000,  // unsupported op (shift) -> stop
        };
        int count = 0;
        void *entry = pd_jit_translate_ra(&j, prog, 0x07000000, 32, &count);
        pd_jit_flush(&j);

        int lpass = 0, ltotal = 0;
        ltotal++; if (count == 9) lpass++;
        pd->system->logToConsole("  t10 translated %d insns (exp 9) %s", count, count == 9 ? "ok" : "BAD");

        if (entry) {
            uint32_t reg[64];
            memset(reg, 0, sizeof(reg));
            ((void (*)(uint32_t *))entry)(reg);
            struct { int idx; uint32_t exp; const char *nm; } chk[] = {
                {7,  0x00001234, "r7(LD_W)"},
                {9,  0xFFFFFFFF, "r9(LD_B)"},
                {10, 0x000000FF, "r10(IN_B)"},
            };
            for (int k = 0; k < 3; k++) {
                ltotal++;
                int good = reg[chk[k].idx] == chk[k].exp;
                if (good) lpass++;
                pd->system->logToConsole("  t10 %s=0x%x exp 0x%x %s",
                                         chk[k].nm, (unsigned)reg[chk[k].idx],
                                         (unsigned)chk[k].exp, good ? "ok" : "BAD");
            }
        }

        // Restore the WRAM we clobbered.
        g_rv_wword(0x05000000, save0);
        g_rv_wword(0x05000004, save4);

        pd->system->logToConsole("pd_jit_test: load/store self-test %d/%d -> %s",
                                 lpass, ltotal, lpass == ltotal ? "LS OK" : "LS FAIL");
        pd_jit_free(&j);
    }

    // --- test 11: branch-terminated blocks return the next PC (Stage 3) ---
    {
        jit_state j;
        if (!pd_jit_init(&j, pd, 8192, 64)) {
            pd->system->logToConsole("pd_jit_test: jit init (branch) failed");
            return;
        }
        int bpass = 0, btotal = 0;

        // A) BE taken (Z=1): MOV_I r5,#0; CMP r5,r5; BE +0x10
        static const uint16_t pA[] = { 0x40A0, 0x0CA5, 0x8410 };
        // B) BE not taken (Z=0): MOV_I r5,#1; CMP r5,r0; BE +0x10
        static const uint16_t pB[] = { 0x40A1, 0x0CA0, 0x8410 };
        // C) BR +0x20 (unconditional)
        static const uint16_t pC[] = { 0x8A20 };
        // D) MOVEA r5,r0,#0x42; JMP r5
        static const uint16_t pD[] = { 0xA0A0, 0x0042, 0x1805 };
        // E) JAL +0x40  (r31 = branch+4, PC = branch+0x40)
        static const uint16_t pE[] = { 0xAC00, 0x0040 };

        struct { const uint16_t *p; uint32_t pc, exp; const char *nm; } cs[] = {
            { pA, 0x07000000, 0x07000014, "BE taken"     },
            { pB, 0x07000100, 0x07000106, "BE not taken" },
            { pC, 0x07000200, 0x07000220, "BR"           },
            { pD, 0x07000300, 0x00000042, "JMP r5"       },
            { pE, 0x07000400, 0x07000440, "JAL"          },
        };
        for (int k = 0; k < 5; k++) {
            int count = 0;
            void *e = pd_jit_translate_ra(&j, cs[k].p, cs[k].pc, 16, &count);
            pd_jit_flush(&j);
            btotal++;
            if (e) {
                uint32_t reg[64];
                memset(reg, 0, sizeof(reg));
                uint32_t nextpc = ((uint32_t (*)(uint32_t *))e)(reg);
                int good = (nextpc == cs[k].exp);
                if (good) bpass++;
                pd->system->logToConsole("  t11 %s next=0x%x exp 0x%x %s",
                                         cs[k].nm, (unsigned)nextpc, (unsigned)cs[k].exp,
                                         good ? "ok" : "BAD");
                if (k == 4) { // JAL also links r31
                    btotal++;
                    int g2 = (reg[31] == 0x07000404);
                    if (g2) bpass++;
                    pd->system->logToConsole("  t11 JAL r31=0x%x exp 0x07000404 %s",
                                             (unsigned)reg[31], g2 ? "ok" : "BAD");
                }
            } else {
                pd->system->logToConsole("  t11 %s: translate failed BAD", cs[k].nm);
            }
        }

        pd->system->logToConsole("pd_jit_test: branch self-test %d/%d -> %s",
                                 bpass, btotal, bpass == btotal ? "BRANCH OK" : "BRANCH FAIL");
        pd_jit_free(&j);
    }
}

#else  // simulator / non-device

void pd_jit_test(PlaydateAPI *pd) {
    pd->system->logToConsole("pd_jit_test: skipped (not on device)");
}

#endif
