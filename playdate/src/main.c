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
#include "pd_video.h"
#include "pd_jit_test.h"
#include "rom_picker.h"

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg);

static int update(void* userdata);

static const char* SYSTEM_FONT_PATH =
    "/System/Fonts/Asheville-Sans-14-Bold.pft";

static LCDFont* s_font = NULL;
static const char* s_load_err = NULL;

// ROM library lives in the shared cross-app emulation tree (CrankBoy
// convention), scanned by pd-rom-picker. The picker creates the folder on first
// run, so a fresh install lands the user in a known place. ROMs are .vb files.
static const char* ROM_FOLDER = "/Shared/Emulation/vb/games/";
static const char* ROM_EXTENSIONS[] = { "vb", NULL };

// App runs in one of two modes: the ROM picker (browse/select), or the running
// emulator. We start in the picker; selecting a ROM loads it and switches to
// EMU. A system-menu item switches back to the picker mid-game.
typedef enum { APP_PICKER, APP_EMU } AppMode;
static AppMode s_mode = APP_PICKER;

static void start_picker(PlaydateAPI* pd);

// Fired synchronously by the picker (either auto-load-single during init, or an
// A-press during rom_picker_update) with an absolute path under ROM_FOLDER.
static void on_rom_picked(const char* path, void* userdata) {
    PlaydateAPI* pd = (PlaydateAPI*)userdata;
    int rc = pd_core_load_rom(pd, path);
    if (rc != 0) {
        // Stay in the picker so the user can choose another file.
        pd->system->logToConsole("on_rom_picked: load '%s' failed rc=%d",
                                  path, rc);
        return;
    }
    // The picker painted the whole panel; make the first emulator blit refresh
    // the full display so its pixels don't linger in the 8 px margins.
    pd_video_request_full_redraw();
    s_mode = APP_EMU;
}

static void start_picker(PlaydateAPI* pd) {
    RomPickerConfig cfg = {
        .folder           = ROM_FOLDER,
        .extensions       = ROM_EXTENSIONS,
        .on_select        = on_rom_picked,
        .userdata         = pd,
        .auto_load_single = 1,
    };
    s_mode = APP_PICKER;
    // rom_picker_init may call on_rom_picked synchronously (auto-load-single),
    // which flips s_mode to APP_EMU — so this must run after pd_core is ready.
    rom_picker_init(pd, &cfg);
    // Selection already happened (auto-load-single): release the picker's
    // file-list heap. Done here, after init returns, because the path passed
    // to on_rom_picked points into that list.
    if (s_mode == APP_EMU) rom_picker_free();
}

// Frame-skip "feel" mode, adjustable at runtime via the Playdate system menu
// (the menu button, so it doesn't steal game input). The interpreter runs every
// Playdate frame so VB game timing/logic stay correct; only the visible render
// (VIP composite + blit) is skipped. For render-bound games this keeps the game
// running at real speed while the screen updates less often. 0 = render every
// frame; N = render every (N+1)th frame.
static int s_render_skip = 1;
static PDMenuItem* s_skip_item = NULL;

static void skip_menu_cb(void* userdata) {
    PlaydateAPI* pd = (PlaydateAPI*)userdata;
    if (s_skip_item) s_render_skip = pd->system->getMenuItemValue(s_skip_item);
}

// "ROM Picker" system-menu item: drop back to the picker to choose another ROM
// mid-game. The current emulation just stops being driven; picking a new ROM
// reloads the core and resumes in EMU mode.
static void picker_menu_cb(void* userdata) {
    PlaydateAPI* pd = (PlaydateAPI*)userdata;
    start_picker(pd);
}

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
        }

        // Dynarec feasibility probe (device-only): confirm we can run
        // generated Thumb-2 code before building the emitter.
        pd_jit_test(pd);

        // Feel test: uncap to the panel's 50 Hz so emulation runs as fast as
        // it can (the old 20 fps cap was throttling simple scenes to 40%
        // speed for no reason). Real game speed is still emulation-bound.
        pd->display->setRefreshRate(50.0f);
        pd->system->setUpdateCallback(update, pd);

        // "Frame skip" feel-mode selector in the system menu (menu button).
        static const char* skip_opts[] = { "Off", "Skip 1", "Skip 2", "Skip 3" };
        s_skip_item = pd->system->addOptionsMenuItem(
            "Frame skip", skip_opts, 4, skip_menu_cb, pd);
        // Default to "Skip 1" so the menu UI matches s_render_skip's initial
        // value (addOptionsMenuItem otherwise starts the selector at "Off").
        if (s_skip_item) pd->system->setMenuItemValue(s_skip_item, s_render_skip);

        // "ROM Picker" item to switch ROMs without relaunching.
        pd->system->addMenuItem("ROM Picker", picker_menu_cb, pd);

        // Start in the ROM picker (unless pd_core_init failed). May auto-load
        // and jump straight to EMU mode if exactly one ROM is present.
        if (!s_load_err) start_picker(pd);
    }

    return 0;
}

// Per-window profile accumulators. One window = LOG_INTERVAL frames; at the
// end of each window we emit one console line summarising avg/max costs.
// Stays out of the hot path: just integer/float adds, one snprintf+log per
// window. logToConsole sent at ~1 Hz; even if it blocks briefly, only one
// frame per second pays for it.
#define LOG_INTERVAL 20

static struct {
    int     count;
    float   total_sum,  total_max;
    float   interp_sum;
    float   vip_sum,    vip_max;
    float   blit_sum;
    int     rendered_frames;
} s_prof;

static void prof_reset(void) {
    s_prof.count = 0;
    s_prof.total_sum = 0.0f; s_prof.total_max = 0.0f;
    s_prof.interp_sum = 0.0f;
    s_prof.vip_sum = 0.0f;   s_prof.vip_max = 0.0f;
    s_prof.blit_sum = 0.0f;
    s_prof.rendered_frames = 0;
}

static int update(void* userdata)
{
    PlaydateAPI* pd = (PlaydateAPI*)userdata;
    const pd_core_status* st = pd_core_get_status();

    if (s_load_err) {
        // One-shot draw of the error and stop running the loop body. No
        // further compute, no further logging.
        pd->graphics->clear(kColorWhite);
        if (s_font) pd->graphics->setFont(s_font);
        char line[96];
        snprintf(line, sizeof(line), "ROM error: %s", s_load_err);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 40);
        snprintf(line, sizeof(line), "folder: %s", ROM_FOLDER);
        pd->graphics->drawText(line, strlen(line), kASCIIEncoding, 8, 64);
        return 1;
    }

    // ROM picker mode: it draws and handles input itself; on A it fires
    // on_rom_picked, which loads the ROM and flips us to APP_EMU. Once that
    // happens, free the picker's file-list heap (after the lib call returns —
    // the selected path points into that list while the callback runs).
    if (s_mode == APP_PICKER) {
        rom_picker_update();
        if (s_mode == APP_EMU) rom_picker_free();
        return 1;
    }

    if (!st->loaded) return 1;

    static int s_skip_counter = 0;
    bool do_render = (s_skip_counter == 0);
    s_skip_counter = (s_skip_counter + 1) % (s_render_skip + 1);

    unsigned int t_start = pd->system->getCurrentTimeMilliseconds();

    pd_core_run_frame(pd);
    float interp_ms = st->last_frame_ms;

    float vip_ms = 0.0f;
    float blit_ms = 0.0f;
    if (do_render) {
        unsigned int t_pre_vip = pd->system->getCurrentTimeMilliseconds();
        pd_video_vip_step(pd);
        unsigned int t_post_vip = pd->system->getCurrentTimeMilliseconds();
        pd_video_blit(pd);
        unsigned int t_post_blit = pd->system->getCurrentTimeMilliseconds();
        vip_ms  = (float)(t_post_vip  - t_pre_vip);
        blit_ms = (float)(t_post_blit - t_post_vip);
        s_prof.rendered_frames++;
    }

    unsigned int t_end = pd->system->getCurrentTimeMilliseconds();
    float total_ms = (float)(t_end - t_start);

    // Built-in game-speed FPS counter (the update-callback rate = VB frames
    // advanced/sec). Drawn after the blit so the frame's memset doesn't wipe it.
    pd->system->drawFPS(2, 2);

    s_prof.count++;
    s_prof.total_sum  += total_ms;
    s_prof.interp_sum += interp_ms;
    s_prof.vip_sum    += vip_ms;
    s_prof.blit_sum   += blit_ms;
    if (total_ms > s_prof.total_max) s_prof.total_max = total_ms;
    if (vip_ms   > s_prof.vip_max)   s_prof.vip_max   = vip_ms;

    if (s_prof.count >= LOG_INTERVAL) {
        // Average over the whole window for interp+total (every frame), but
        // only over rendered frames for vip/blit (so the per-render cost is
        // visible even when RENDER_SKIP > 0).
        float inv_all = 1.0f / (float)s_prof.count;
        float inv_ren = s_prof.rendered_frames > 0
                      ? 1.0f / (float)s_prof.rendered_frames
                      : 0.0f;
        pd->system->logToConsole(
            "f=%lu int=%.1f vip=%.1f blit=%.1f tot=%.1f (max tot=%.1f vip=%.1f) "
            "rendered=%d/%d cy=%lu PC=%08lx XP=%04x",
            (unsigned long)st->frames_run,
            (double)(s_prof.interp_sum * inv_all),
            (double)(s_prof.vip_sum    * inv_ren),
            (double)(s_prof.blit_sum   * inv_ren),
            (double)(s_prof.total_sum  * inv_all),
            (double)s_prof.total_max,
            (double)s_prof.vip_max,
            s_prof.rendered_frames, s_prof.count,
            (unsigned long)st->last_frame_cycles,
            (unsigned long)st->pc,
            st->xpctrl);
        prof_reset();
    }

    return 1;
}
