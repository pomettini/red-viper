// Playdate-side glue around the Red Viper V810 core.

#ifndef PD_CORE_H
#define PD_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pd_api.h"

typedef struct {
    bool        loaded;            // true once a ROM is mapped and the CPU is reset
    int         rom_size;          // bytes of ROM data actually loaded
    int         last_run_ret;      // return value of v810_run() last frame
    uint32_t    last_frame_cycles; // emulated cycles in the last run_frame
    uint32_t    total_cycles;      // emulated cycles since reset
    float       last_frame_ms;     // measured wall-clock ms in the last run_frame
    uint32_t    pc;                // V810 PC after last run
    uint16_t    xpctrl;            // tVIPREG.XPCTRL after last run
    uint16_t    intpnd;            // tVIPREG.INTPND after last run
    uint32_t    frames_run;        // count of VB frames the interpreter has produced
} pd_core_status;

// Returns 0 on success; negative on failure. Diagnostics printed via pd->system->logToConsole.
int pd_core_init(PlaydateAPI *pd);

// Load a ROM bundled in the .pdx's Source/. path is relative to the bundle
// root (e.g. "warioland.vb"). Calls v810_reset after loading.
int pd_core_load_rom(PlaydateAPI *pd, const char *path);

// Run one Virtual Boy frame (i.e. v810_run until the emulator yields).
// Updates pd_core_get_status().
void pd_core_run_frame(PlaydateAPI *pd);

const pd_core_status *pd_core_get_status(void);

#endif // PD_CORE_H
