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
}

#else  // simulator / non-device

void pd_jit_test(PlaydateAPI *pd) {
    pd->system->logToConsole("pd_jit_test: skipped (not on device)");
}

#endif
