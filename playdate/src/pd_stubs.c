// Playdate-side stubs for symbols the Red Viper core expects from the 3DS
// frontend / sound / video / DRC layers. v0 keeps these no-ops so the
// interpreter can run; real Playdate implementations replace them later.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "vb_types.h"
#include "vb_set.h"
#include "vb_dsp.h"
#include "v810_cpu.h"
#include "v810_mem.h"
#include "drc_core.h"

// ---------------------------------------------------------------------------
// Settings: a single zeroed VB_OPT. Adjusted by pd_core_init().

VB_OPT tVBOpt;
int vbkey[32];

void setCustomMappingDefaults(void) {}
void setDefaults(void) {}
int loadFileOptions(void) { return 0; }
int saveFileOptions(void) { return 0; }
int loadGameOptions(void) { return 0; }
int saveGameOptions(void) { return 0; }
int deleteGameOptions(void) { return 0; }

// ---------------------------------------------------------------------------
// Multiplayer plumbing the core touches via functions. The globals
// (is_multiplayer, emulating_self, my_player_id, emulated_player_id) are
// defined in v810_cpu.c — don't redefine.

void vblink_init(void) {}

// ---------------------------------------------------------------------------
// DRC stubs. With DRC_AVAILABLE=false (see drc_core.h) drc_run/init/reset are
// never reached. drc_handleInterrupts and drc_relocTable are still bound to
// cpu_state function pointers in v810_reset; they must exist as symbols but
// should never be invoked through the interpreter path.

void drc_init(void) {}
void drc_reset(void) {}
void drc_exit(void) {}
int  drc_run(void) { return DRC_ERR_NO_DYNAREC; }
void drc_loadSavedCache(void) {}
void drc_dumpCache(char *filename) { (void)filename; }
void drc_dumpDebugInfo(int code) { (void)code; }
void drc_clearCache(void) {}

int drc_handleInterrupts(WORD cpsr, WORD *PC) {
    (void)cpsr; (void)PC;
    return 0;
}

void drc_relocTable(void) {}

// ---------------------------------------------------------------------------
// Video. The cached state (tDSPCACHE) is provided by video_common.c, which we
// do link in. The 3DS GPU pipeline (video.c / video_hard.c / video_soft.cpp)
// is replaced by these no-ops. Once a Playdate renderer exists it lives in
// pd_video.c and these stubs go away.

// tDSPCACHE and eye_count are normally defined in source/common/video.c, which
// we don't link in (it's the 3DS GPU pipeline). Define them here so v810_mem.c
// dirty-flag updates and v810_cpu.c interrupt servicing find them.
VB_DSPCACHE tDSPCACHE;
int eye_count = 1;  // single-eye output on Playdate; see NOTES.md §6

void video_init(void) {}
void video_render(int displayed_fb, bool on_time) { (void)displayed_fb; (void)on_time; }
void video_flush(bool default_for_both) { (void)default_for_both; }
void video_quit(void) {}
void video_download_vip(int drawn_fb) { (void)drawn_fb; }
void video_hard_render(int drawn_fb) { (void)drawn_fb; }
// video_soft_render and update_texture_cache_soft come from
// source/common/video_soft.cpp (compiled via core/video_soft.cpp symlink).
void update_texture_cache_hard(void) {}

// ---------------------------------------------------------------------------
// Sound. VSU emulation is not pulled in for v0 — we stub the entry points so
// memory writes from CPU code into the sound register region are absorbed.

void sound_init(void) {}
void sound_close(void) {}
void sound_pause(void) {}
void sound_resume(void) {}
void sound_reset(void) {}
void sound_refresh(void) {}
void sound_update(uint32_t cycles) { (void)cycles; }
void sound_write(int addr, uint16_t data) { (void)addr; (void)data; }

// ---------------------------------------------------------------------------
// Replay. Not used in v0.

void replay_init(void) {}
void replay_reset(bool sram) { (void)sram; }

// ---------------------------------------------------------------------------
// Input. The core sometimes refers to V810_RControll through the hardware
// register read path. Return "no buttons pressed" for v0.

void input_init(void) {}

HWORD V810_RControll(bool reset) {
    (void)reset;
    return 0x0002; // 0x0002 = "no buttons" sentinel per VB hardware
}

// ---------------------------------------------------------------------------
// GUI / app menu. Not implemented for v0.

void guiInit(void) {}
void openMenu(bool first) { (void)first; }
void toggleAnaglyph(bool on) { (void)on; }
void toggleVsync(bool on) { (void)on; }
