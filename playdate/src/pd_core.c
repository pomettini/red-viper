#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "pd_api.h"
#include "pd_core.h"

#include "vb_types.h"
#include "vb_set.h"
#include "vb_dsp.h"
#include "v810_cpu.h"
#include "v810_mem.h"
#include "rom_db.h"
#include "pd_itcm.h"
#ifdef RV_JIT
#include "pd_jit_run.h"
#endif

extern void apply_patches(void);

// Map current Playdate button state to a VB input HWORD and write it into
// tHReg.SLB / tHReg.SHB so the next v810_run sees it. The 0x0002 bit is the
// "controller ID present" sentinel the VB always sees from a connected pad.
//
// Playdate has only A/B/D-pad. Start/Select don't exist as physical buttons
// here; we map them creatively:
//   - Playdate A    -> VB A
//   - Playdate B    -> VB B
//   - Playdate D-pad-> VB Left D-pad
//   - A + B together-> VB Start (held briefly to advance title screens)
// (Right D-pad and Select are post-MVP.)
static void pd_update_input(PlaydateAPI *pd) {
    PDButtons cur;
    pd->system->getButtonState(&cur, NULL, NULL);

    HWORD k = 0x0002;
    bool a = (cur & kButtonA) != 0;
    bool b = (cur & kButtonB) != 0;
    if (a && b) {
        k |= VB_KEY_START;
    } else {
        if (a) k |= VB_KEY_A;
        if (b) k |= VB_KEY_B;
    }
    if (cur & kButtonUp)    k |= VB_LPAD_U;
    if (cur & kButtonDown)  k |= VB_LPAD_D;
    if (cur & kButtonLeft)  k |= VB_LPAD_L;
    if (cur & kButtonRight) k |= VB_LPAD_R;

    vb_state->tHReg.SLB = (BYTE)k;
    vb_state->tHReg.SHB = (BYTE)(k >> 8);
}

static pd_core_status s_status;

const pd_core_status *pd_core_get_status(void) { return &s_status; }

// Apply a minimal set of tVBOpt defaults sufficient for v810_reset / interpreter.
static void pd_apply_default_opts(void) {
    memset(&tVBOpt, 0, sizeof(tVBOpt));
    tVBOpt.RENDERMODE = RM_TOGPU;     // expected default; reset() may override per game
    tVBOpt.FRMSKIP = 0;
    tVBOpt.DEFAULT_EYE = 0;           // left eye
    tVBOpt.VSYNC = false;
    tVBOpt.ANAGLYPH = false;
    tVBOpt.INPUT_BUFFER = 0;
}

int pd_core_init(PlaydateAPI *pd) {
    pd_apply_default_opts();
    v810_init();
    rv_itcm_init(); // route mem accessors through g_rv_* (default: plain mem_*)
#ifdef RV_JIT
    rv_jit_init(pd);
#endif
    s_status.loaded = false;
    pd->system->logToConsole("pd_core_init: V810 state buffers allocated; itcm region=%u bytes",
                             (unsigned)rv_itcm_size());
    return 0;
}

int pd_core_load_rom(PlaydateAPI *pd, const char *path) {
    // kFileReadData lets absolute paths in the shared data tree
    // (/Shared/Emulation/vb/games/...) resolve; kFileRead keeps bundle-relative
    // names working too. The picker hands us absolute /Shared paths.
    SDFile *f = pd->file->open(path, kFileRead | kFileReadData);
    if (!f) {
        pd->system->logToConsole("pd_core_load_rom: open '%s' failed: %s",
                                 path, pd->file->geterr());
        return -1;
    }

    // Seek to end to get the file size.
    pd->file->seek(f, 0, SEEK_END);
    int rom_size = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);

    if (rom_size <= 0x10 || rom_size > MAX_ROM_SIZE) {
        pd->system->logToConsole("pd_core_load_rom: invalid ROM size %d", rom_size);
        pd->file->close(f);
        return -2;
    }
    // Must be power of two.
    if (rom_size & (rom_size - 1)) {
        pd->system->logToConsole("pd_core_load_rom: ROM size %d not power of two",
                                 rom_size);
        pd->file->close(f);
        return -3;
    }

    int got = pd->file->read(f, V810_ROM1.pmemory, rom_size);
    pd->file->close(f);
    if (got != rom_size) {
        pd->system->logToConsole("pd_core_load_rom: short read %d/%d", got, rom_size);
        return -4;
    }

    V810_ROM1.size = rom_size;
    V810_ROM1.highaddr = 0x07000000 + rom_size - 1;

    // Fill the rest of the address space with copies of the ROM (mirrors).
    for (int i = rom_size; i < MAX_ROM_SIZE; i += rom_size) {
        memcpy(V810_ROM1.pmemory + i, V810_ROM1.pmemory, rom_size);
    }

    is_sram = false;
    gen_table();
    tVBOpt.CRC32 = get_crc(rom_size);
    tVBOpt.GAME_ID = MAKE_GAMEID((char*)(V810_ROM1.off + (V810_ROM1.highaddr & 0xFFFFFDF9)));

    apply_patches();
    v810_reset();

    s_status.loaded = true;
    s_status.rom_size = rom_size;
    s_status.last_run_ret = 0;
    s_status.last_frame_cycles = 0;
    s_status.last_frame_ms = 0.0f;

    pd->system->logToConsole("pd_core_load_rom: '%s' loaded, %d bytes, crc=%08lx, gid=%08x",
                             path, rom_size,
                             (unsigned long)tVBOpt.CRC32,
                             (unsigned)tVBOpt.GAME_ID);
    return 0;
}

void pd_core_run_frame(PlaydateAPI *pd) {
    if (!s_status.loaded) return;

    pd_update_input(pd);

#ifdef RV_JIT
    // Make blocks translated during the previous frame executable (batched
    // I-cache flush). Must precede v810_run so this frame can run them.
    rv_jit_frame_flush();
#endif

    // Relocate the hot V810 memory accessors into a stack buffer (DTCM on
    // device) for this frame, then run the whole frame within this scope so
    // the buffer stays live and g_rv_* keep pointing into it. The 3DS-style
    // technique from the Beetle VB port; no-op (returns 0) off-device.
    uint8_t itcm_buf[4096] __attribute__((aligned(32)));
    static int s_itcm_logged = 0;
    int relocated = rv_itcm_relocate(itcm_buf, sizeof(itcm_buf));
    if (!s_itcm_logged) {
        s_itcm_logged = 1;
        pd->system->logToConsole("pd_itcm: relocate=%d (region=%u bytes, buf=%u)",
                                 relocated, (unsigned)rv_itcm_size(), (unsigned)sizeof(itcm_buf));
    }

    uint32_t start_cycles = vb_state->v810_state.cycles;
    pd->system->resetElapsedTime();

    int ret = v810_run();

    float elapsed = pd->system->getElapsedTime();
    s_status.last_frame_ms = elapsed * 1000.0f;
    s_status.last_frame_cycles = vb_state->v810_state.cycles - start_cycles;
    s_status.total_cycles += s_status.last_frame_cycles;
    s_status.last_run_ret = ret;
    s_status.pc = vb_state->v810_state.PC;
    s_status.xpctrl = vb_state->tVIPREG.XPCTRL;
    s_status.intpnd = vb_state->tVIPREG.INTPND;
    s_status.frames_run++;
}
