// Red Viper — Playdate port, v0.1.
//
// Wires the Red Viper V810 interpreter to a Playdate event handler. No
// renderer yet; we just run one VB frame per Playdate update tick and draw
// timing telemetry. The point at this stage is the first measurable number:
// "how long does interpreter-only Red Viper take per VB frame on Playdate?".

#include <stdio.h>
#include <string.h>

#include "pd_api.h"
#include "pd_core.h"

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg);

static int update(void* userdata);

static const char* SYSTEM_FONT_PATH =
    "/System/Fonts/Asheville-Sans-14-Bold.pft";

static LCDFont* s_font = NULL;
static const char* s_load_err = NULL;

static const char* ROM_PATH = "warioland.vb";

int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
    (void)arg;

    if (event == kEventInit) {
        const char* err = NULL;
        s_font = pd->graphics->loadFont(SYSTEM_FONT_PATH, &err);
        if (!s_font) {
            pd->system->error("loadFont %s failed: %s",
                              SYSTEM_FONT_PATH, err ? err : "(null)");
        }

        if (pd_core_init(pd) != 0) {
            s_load_err = "pd_core_init failed";
        } else {
            int rc = pd_core_load_rom(pd, ROM_PATH);
            if (rc != 0) {
                static char buf[64];
                snprintf(buf, sizeof(buf), "load_rom rc=%d", rc);
                s_load_err = buf;
            }
        }

        // 20 fps target (50 ms budget). See playdate/NOTES.md §5.
        pd->display->setRefreshRate(20.0f);
        pd->system->setUpdateCallback(update, pd);
    }

    return 0;
}

static int update(void* userdata)
{
    PlaydateAPI* pd = (PlaydateAPI*)userdata;
    const pd_core_status* st = pd_core_get_status();

    pd_core_run_frame(pd);

    pd->graphics->clear(kColorWhite);
    if (s_font) pd->graphics->setFont(s_font);

    char line[96];

    snprintf(line, sizeof(line), "Red Viper PD - v0.1 benchmark");
    pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 8);

    if (s_load_err) {
        snprintf(line, sizeof(line), "ROM error: %s", s_load_err);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 40);
        snprintf(line, sizeof(line), "path: %s", ROM_PATH);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 64);
    } else if (!st->loaded) {
        snprintf(line, sizeof(line), "loading...");
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 40);
    } else {
        snprintf(line, sizeof(line), "ROM: %s (%d bytes)", ROM_PATH, st->rom_size);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 40);
        snprintf(line, sizeof(line), "Last frame: %.2f ms", st->last_frame_ms);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 64);
        snprintf(line, sizeof(line), "Emulated cycles: %lu",
                 (unsigned long)st->last_frame_cycles);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 88);
        snprintf(line, sizeof(line), "v810_run ret: %d", st->last_run_ret);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 112);
        snprintf(line, sizeof(line), "Beetle VB ref: 28-70 ms warm");
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 160);
    }

    pd->system->drawFPS(0, 0);
    return 1;
}
