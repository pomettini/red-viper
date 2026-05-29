# Red Viper → Playdate port — engineering notes

A live log of decisions, measurements, and dead ends for the experimental
Playdate port of Red Viper. Updated in place as work progresses.

## 1. Why this exists / what we are trying to learn

Goal: determine whether Red Viper's architecture (V810 dynamic recompiler,
busywait detection, cached renderer) can make Virtual Boy emulation reach
playable speed on Playdate hardware, at least for less demanding games.
Full-speed accuracy is **not** a goal — we want a credible technical answer to
"can this be made playable?", and a runnable artifact if yes.

Beetle VB was already ported to Playdate and **failed to reach real-time**
(28–70 ms/frame vs. 20 ms budget). That postmortem
(`github.com/pomettini/beetle-vb-libretro`, NOTES.md) is the primary input.

## 2. Beetle VB postmortem — what it tells us

Verbatim summary from the Beetle VB NOTES.md:

- **Root cause was memory latency, not CPU compute.** ~80 % of frame time on
  Wario Land was spent stalled on SDRAM instruction fetches. Cortex-M7 has a
  16 KB D-cache; a 2 MB game working set thrashes it almost completely.
- Measured ~55 ARM cycles per emulated V810 cycle on heavy scenes; the budget
  is closer to ~9.
- A bespoke V810→Thumb-2 JIT was added. **Warm cache: 28–30 ms** (near
  playable). **Cold/eviction: 68–80 ms**. **First frame: 1693 ms**. The
  256 KB code cache held only ~640 average blocks — too small.
- Display rewrite (column-major 2 bpp → row-major 8 bpp, single eye) dropped
  rendering to **13–30 ms**, leaving rendering as a secondary cost.
- Interpreter dispatch overhead was ~15 % of frame time. Eliminating it
  entirely would not have closed the gap.
- DTCM was unusable for emulator state — ~7.7 KB available after OS overhead.
- Conclusions: real-time would require (a) hand-tuned ARM ASM interpreter
  with explicit load scheduling, (b) a much larger JIT with inlined memory
  access + block chaining + larger code cache, or (c) offline static
  translation.

## 3. Why Red Viper is a better starting base than Beetle VB

| Concern | Beetle VB | Red Viper |
|---|---|---|
| CPU strategy | Interpreter-first; JIT bolted on later | DRC-first (`source/arm/drc_*`), interpreter is a fallback when PC leaves ROM |
| Memory access in hot path | Indirected via callbacks | Inlined per opcode at translation time via `arm_emit.h` / `arm_codegen.h` |
| Block linking | Limited / late addition | First-class (`drc_executeBlock` exits via `pop {pc}` to a chained block address) |
| Busywait detection | None reported | `v810_instruction.busywait` flag set during translation; idle spins collapse to a cycle skip |
| Renderer architecture | Whole-frame VIP rasteriser per frame | Cached BG/Obj/Char data with dirty bits (`tDSPCACHE`) so only changed tiles re-rasterise |
| Cycle/timing model | Per-instruction in interpreter | Per-block cycle totals + `cycles_until_event` partial/full window (`source/arm/drc_exec.s` post-block bookkeeping) |
| Target hardware | 3DS ARM11 (A32-capable) | 3DS ARM11 (A32-capable) — same problem for us |

The architectural decisions that gave Red Viper full-speed 3DS performance
(DRC, dirty-cache renderer, busywait detection) are exactly the techniques
the Beetle VB postmortem says we need. We start with all three.

## 4. Architectural assessment — what Red Viper does, and where it breaks on Playdate

### What can be reused (mostly verbatim)

- `source/common/v810_cpu.[ch]` — V810 CPU state, init, interrupt service.
- `source/common/v810_ins.c` — instruction helpers (`ins_rev`, divmod, BSTR
  sub-ops). Plain C, no platform deps.
- `source/common/v810_mem.[ch]` — VIP / WRAM / SRAM / ROM dispatch. Plain C
  except it pulls in `vb_dsp.h` (which currently `#include`s `citro3d.h`
  under `__3DS__`).
- `source/common/interpreter.c` — pure C V810 interpreter. **This is the
  Playdate fallback / day-one CPU**, before any Thumb-2 backend exists.
- `source/common/patches.c`, `rom_db.c`, `replay.c`, `vb_set.c` — plain C
  utility/state code, easy to port.
- `source/common/vb_sound.c` — VSU emulation, plain C. Output stage will be
  rewritten for Playdate audio.

### What must be rewritten

- **The dynarec backend (`source/arm/*`)** is the big one. It emits **ARM
  A32** instructions (see `drc_exec.s`: `.arm`, `msr cpsr_f`, conditional
  data-processing, ARM-encoded `ldr ... lsr #N`). Cortex-M7 is **Thumb-2
  only** — it physically cannot decode ARM mode. The Beetle VB port had to
  write a Thumb-2 emitter from scratch; we will too. Effort estimate:
  comparable to or larger than Beetle VB's JIT (Red Viper's emitter is
  richer: register allocation, branch optimisation, busywait, BSTR
  relocations, FPP). On the upside, we get to keep the V810 decode and IR
  in `drc_core.c` largely unchanged — only the `arm_emit_*` / `arm_codegen_*`
  layer plus `drc_exec.s` need replacement.
- **Renderer.** `source/common/video_hard.c` is PICA200 / citro3d — useless.
  `source/common/video_soft.cpp` is closer to portable (it composites into a
  2 bpp software framebuffer per eye with column-table dither), but its
  output target is a `C3D_Tex`. Plan: write a fresh `video_pd.c` that
  composites **one eye** (right is discarded — see §6) directly into a
  1 bpp Playdate frame buffer with a chosen reduction function.
- **Frontend** (`source/3ds/*`) — replaced by `playdate/src/*`. The 3DS GUI,
  multiplayer (`vblink`), and config UI are out of scope for v0.
- **Sound output.** VSU emulation core stays; the 3DS DSP submission in
  `source/3ds/sound.c` is replaced by Playdate `playdate->sound` API.
- **Input.** `source/3ds/input.c` is replaced by Playdate `getButtonState` /
  crank polling.

### What depends on hardware features Playdate lacks

- **3D / stereo.** Playdate's display is 1-bit 400×240 mono. The Red Viper
  3D pipeline (parallax, depth slider, anaglyph) is meaningless. We render
  **left eye only**, full stop, in v0.
- **PICA200 GPU.** No analogue. The dirty-cache rasterisation logic in
  `video_common.c` / `video_soft.cpp` is the salvageable software path.
- **DSP for audio.** Playdate has a 44.1 kHz software mixer; fine, but
  buffering and frame pacing will differ.
- **DTCM for hot data.** Beetle VB measured ~7.7 KB free after RTOS overhead
  — likely the same here. **Action: measure on real hardware before
  assuming we can put anything significant in DTCM.** Even 8 KB is enough
  for the V810 register file and a small dispatch stub if we're careful.

### What should be measured first

In priority order — the things that, if they don't work, kill the project:

1. **Interpreter baseline.** Get `interpreter_run()` looping on a real ROM,
   measure ms/frame for a static scene (title screen). Expect very slow
   (worse than Beetle VB's interpreter), but it's our control point.
2. **WRAM-in-DTCM feasibility.** Beetle VB couldn't. We must re-test —
   Playdate SDK exposes `__attribute__((section(".dtcm")))` placement and
   the C_API setup zeroes BSS. If we get even 64 KB DTCM, putting
   `V810_VB_RAM` (64 KB) there would meaningfully change the cost curve.
3. **ROM placement.** Can we map the 2 MB ROM into the executable's flash
   region (`.rodata`) at build time, or must it live in SDRAM? Flash on the
   Playdate is slower per access than SRAM but cacheable; SDRAM is the
   thing the postmortem says costs us. If a ROM-bundled forwarder model is
   acceptable for v0, `.rodata` may be a win. (Cartridge load goes to
   SDRAM regardless.)
4. **Dynarec viability sketch.** Estimate Thumb-2 backend effort before
   committing. If we can produce 10–20 V810 ops via Thumb-2 by hand and
   verify the rest is mechanical, proceed. If not, fall back to
   threaded-interpreter / cached-block-interpreter approach.
5. **Renderer minimum cost.** How fast can a single-eye composite into a
   1 bpp 384×224-cropped framebuffer run on Cortex-M7 with realistic dirty
   regions? If the rasteriser eats >5 ms/frame with no CPU emulation cost
   at all, the budget is already gone.

## 5. Decisions taken at v0

These are starting positions, not final answers. Revise as data comes in.

1. **Live in `playdate/`**, not in-tree with 3DS sources. Reusing
   `source/common/*.c` happens via VPATH from `playdate/Makefile` so we
   don't fork the core.
2. **Interpreter-first build.** Day-one binary just runs the interpreter +
   minimal renderer + audio off. Validates that the build pipeline works
   and gives us a perf floor.
3. **Single-eye render, no stereo.** Left eye into a mono 1 bpp buffer.
   Right-eye work is skipped entirely in the rasteriser (no double cost).
4. **No GUI / file browser in v0.** ROM is loaded by name from the .pdx's
   `Source/` data folder. Settings are compile-time constants.
5. **No multiplayer, no replay, no savestates in v0.** Strip everything
   that isn't on the critical perf path.
6. **48 ms / 20 fps target for v0**, not 50 Hz. If we hit 48 ms, we know
   the architecture is in striking range and can chase 20 ms with
   optimisation. If 48 ms is unreachable, the project is over.
7. **Frame skip on overrun**, no waiting. The Playdate display is 1 bpp +
   memory LCD — late frames are visually fine; constant-rate slow is much
   worse than variable-rate not-quite-fast.

## 6. Rendering redesign (sketch — not implemented yet)

- **One eye only** (left). Right-eye composite passes skipped before VRAM
  walk.
- **Crop, do not scale.** VB is 384×224. Playdate is 400×240. Vertical
  centre (8 px top/bottom margin). Horizontal: 8 px left/right margin and
  centre. Margins blank (black or background pattern). No interpolation
  required.
- **2 bpp → 1 bpp.** VB pixels are 4-level (0 / mid-low / mid-high / on).
  Options to evaluate in order of expected cost:
  1. **Threshold at 50 %.** Cheapest. Loses anti-aliasing detail; many VB
     games actually look fine because they use VIP brightness as crude
     anti-aliasing for read-as-on art.
  2. **2×2 ordered Bayer dither.** Cheap. Better gradient preservation,
     same per-pixel cost as threshold + one mask lookup.
  3. **Frame-rate mod (PWM-style).** Toggle low-intensity pixels every N
     frames. Maps VB's column-table brightness onto temporal dither.
     Memory LCD doesn't blur, so this will visibly flicker — avoid unless
     we're locked at 50 Hz.
- **Write directly into Playdate's framebuffer.** Skip intermediate buffers.
  Each row of VB output is 384 px = 48 bytes of 1 bpp; centre into 50 byte
  Playdate row at column offset 1. No row-stride conversion.
- **Skip invisible work early.** Worlds with `WORLD.on == 0` for the left
  eye (`on & 0x2 == 0`) are never composited. Empty BG cells (already
  detected via dirty-tile cache) cost nothing.
- **No column table for v0.** VB's per-column brightness modulation is
  what makes anaglyph 3D work — and we don't have 3D. Drop it; one
  brightness level for the whole frame.

## 7. Open questions / risks

- **Cortex-M7 instruction cache flushing for self-modifying code.**
  Dynarec writes generated code into a buffer then jumps to it.
  Cortex-M7 has separate I/D caches with no automatic coherence. The
  Playdate SDK exposes `__DSB`/`__ISB`/cache maintenance via CMSIS
  (`SCB_CleanDCache_by_Addr`, `SCB_InvalidateICache_by_Addr`). These are
  not free, but per-block they're cheap relative to translation cost.
- **Executable memory.** Playdate apps run from MPU-controlled memory.
  We need a region that is both writable (for code emit) and executable.
  Playdate SDK examples don't do this. Need to confirm what's actually
  permitted — possibly via the linker script `link_map.ld` to carve a
  `.code_cache` section.
- **Heap size.** `Makefile` shows `HEAP_SIZE = 8388208` (~8 MB) is
  typical. Red Viper allocates `MAX_ROM_SIZE = 0x1000000` (16 MB) for ROM
  alone. **Even after restricting to small ROMs we need 2 MB for ROM and
  ~256–512 KB for a code cache.** Have to reduce `MAX_ROM_SIZE` to a
  realistic cap (4 MB? 2 MB?) and accept some games won't load.
- **No FPU emulation issue.** Cortex-M7 has hardware FP single-precision
  (`-mfpu=fpv5-sp-d16`); VB's FPP single-precision arithmetic maps
  directly. No emulated math needed.

## 8. Differences from Beetle VB Playdate port

- Start point: dynarec-shaped emulator, not interpreter-shaped emulator.
- Display strategy chosen up front: single eye + crop + threshold/dither,
  not retrofitted.
- Memory layout decisions made before code is written. Beetle VB tried
  DTCM only after profiling forced it.
- Smaller scope intentionally — no GUI, no settings, no SRAM saves in v0.
- We have a baseline number to beat (Beetle VB warm 28–30 ms). If we can
  hit that or better on an interpreter-only build we're already in good
  shape; if Red Viper's dynarec-on-Thumb-2 can break 20 ms warm we win.

## 9. Iteration log

### 2026-05-28 — initial scaffolding

- Created `playdate/` tree. `src/main.c` is a minimal Playdate event
  handler that draws a banner and the build time; no emulator code yet.
- `Makefile` + `CMakeLists.txt` follow the standard Playdate SDK pattern
  (`common.mk` / `playdate_game.cmake`). Source dirs not yet wired to
  `source/common/`; that comes in step 2.
- This NOTES.md and the Beetle VB postmortem summary above were the only
  research outputs of this session. No emulator code compiled yet on
  Playdate target.

### 2026-05-28 — interpreter wired, first build that includes the V810 core

- Linked `source/common/{v810_cpu, v810_ins, v810_mem, interpreter,
  video_common, patches, rom_db}.c` into the Playdate target. Both the
  device (Cortex-M7 Thumb-2 ELF, 694 KB) and simulator (dylib, 135 KB)
  link cleanly; `RedViper.pdx` is packaged with `pdex.bin`, `pdex.dylib`,
  and the bundled `warioland.vb` (2 MB).
- `playdate/src/pd_stubs.c` stubs out everything the core expected from
  `source/3ds/*`: GPU pipeline (`video_init/render/flush/quit`,
  `video_download_vip`, `video_hard/soft_render`, texture-cache
  invalidation), audio output (`sound_init/write/update/refresh`), input
  (`V810_RControll` returns "no buttons"), GUI hooks, replay, multiplayer
  / vblink, DRC entry points (`drc_init/reset/run/handleInterrupts/
  relocTable`). `tDSPCACHE` and `eye_count` (normally defined in `video.c`)
  also live in stubs so dirty-flag updates in `v810_mem.c` have a target.
- `playdate/src/pd_core.c` provides the ROM loader: opens the bundled
  ROM via `pd->file->open`, reads into `V810_ROM1.pmemory`, mirrors the
  ROM through the address space, runs `gen_table` / `get_crc` /
  `apply_patches` / `v810_reset`. CRC and game ID logged to the Playdate
  console.
- `pd_core_run_frame` calls `v810_run` (which dispatches to
  `interpreter_run` since DRC_AVAILABLE is forced false on Playdate) and
  times it via `pd->system->getElapsedTime`. `main.c` runs one VB frame
  per Playdate update tick and draws `ms`, emulated cycles, and the
  `v810_run` return code.
- Core changes outside `playdate/`:
  - `include/drc_core.h`: `DRC_AVAILABLE` is forced false when
    `TARGET_PLAYDATE` or `TARGET_SIMULATOR` is defined. (Both must be
    handled because the SDK simulator clang invocation only defines
    `TARGET_SIMULATOR`.)
  - `source/common/v810_cpu.c`: `<minizip/unzip.h>` include and the
    `v810_load_init/step/cancel` stdio loaders are gated out for the
    Playdate targets. `v810_reset`'s `replay_reset(is_sram || load_sram)`
    falls back to `replay_reset(is_sram)` (no `load_sram` symbol exists
    once the stdio path is gated).
  - These are the only modifications to shared sources; the 3DS port is
    unaffected (it doesn't define `TARGET_PLAYDATE` or `TARGET_SIMULATOR`).
- Build-system surprise documented: SDK's makefile pattern produces object
  paths by prefixing `build/` to the source path, so `../source/common/
  v810_cpu.c` ends up as `build/../source/common/v810_cpu.o`, which
  resolves to `<repo>/source/common/v810_cpu.o`. On macOS that aliases
  with the bundle's `Source/` directory and `pdc` packages the stray
  objects. Fix: created `playdate/core/` with symlinks to
  `../source/common/*.c` so SRC paths stay rooted inside `playdate/`,
  and `.o` files land cleanly under `build/core/`.
- **Not yet done / what we know:** no renderer (`pd_stubs.c::video_*` are
  no-ops), no input, no audio. The CPU runs, but with no VIP framebuffer
  download Wario Land won't render anything visible — we just have a
  ms/frame number. That number is what the next session captures.

### 2026-05-28 — first crash diagnosed: MAX_ROM_SIZE blew the heap

- Simulator force-quit on first launch. No log surfaced in the simulator
  UI because the crash happened before `setUpdateCallback` returned.
- Attached lldb to `Playdate Simulator.app/Contents/MacOS/Playdate
  Simulator` with the `.pdx` as argv and got: `EXC_BAD_ACCESS (code=1,
  address=0x0)` at `_platform_memmove`, frame #1 = `pd_core_load_rom`
  in `pd_core.c:78` (the ROM-mirror loop). Registers x0=0x200000
  (dst), x1=0x0 (src), x2=0x200000 (n) — confirmed `V810_ROM1.pmemory`
  was NULL.
- Root cause: `MAX_ROM_SIZE` in `include/v810_mem.h` is `0x1000000`
  (16 MB). The Playdate Simulator simulates the device heap budget
  (the project's `HEAP_SIZE = 8388208` ≈ 8 MB), so `malloc(16 MB)` in
  `v810_init` returns NULL. The 3DS port doesn't see this because the
  3DS has more headroom.
- Fix: gate `MAX_ROM_SIZE` in `include/v810_mem.h` to **2 MB** under
  `TARGET_PLAYDATE`/`TARGET_SIMULATOR`. 2 MB is the largest officially
  licensed VB ROM (Wario Land) — covers every commercial release.
  The 3DS code path is unchanged. ROMs larger than 2 MB are now
  rejected by `pd_core_load_rom` instead of silently corrupting state.
- Re-run under lldb after rebuild: ROM loaded successfully, CRC
  `133E9372` matches the known Wario Land checksum
  (`source/common/rom_db.c:43`), GAME_ID `901ade10`. No crash; the
  simulator stays running. Patches in `source/common/patches.c` for
  game ID `01VWCJ` (Wario Land J) apply.
- **Lesson for NOTES.md §7 / risks list (heap budget):** confirmed
  on the simulator. The same `malloc(MAX_ROM_SIZE)` will fail on
  hardware too. Future ROM-size policy: load size first, allocate
  exact-fit, only mirror to next power-of-two — saves up to 6 MB on
  256 KB ROMs (most VB titles). Deferred until we need the headroom.

### 2026-05-28 — software VIP renderer + 1bpp blit

- Pulled `source/common/video_soft.cpp` (the Red Viper software VIP
  renderer) into the Playdate build via a symlink at
  `playdate/core/video_soft.cpp`. This is the existing 2bpp column-major
  rasterizer; it does BG-world, affine, and OBJ compositing into
  V810_DISPLAY_RAM at the same offsets the VB hardware writes.
- Removed the `update_texture_cache_soft` and `video_soft_render` stubs
  from `pd_stubs.c` — real implementations now link in.
- Added `playdate/src/pd_video.c` which, per emulated frame:
  1. If XPEN: runs `update_texture_cache_soft` (on `CharCacheInvalid`)
     and `video_soft_render(drawn_fb)`.
  2. Reads the displayed framebuffer (left eye, FB0), threshold-converts
     2bpp → 1bpp (`value >= 2 ⇒ white`), centers into Playdate's
     400×240 with 8 px margins.
  3. `pd->graphics->markUpdatedRows(0, 239)`.
- Hooked `pd_video_render_frame` from `main.c::update`, replacing the
  previous "telemetry-only" screen. Telemetry now shows as a compact
  overlay at y=220 (`%.1fms %lu cy ret=%d`).
- Build-system surgery (documented for future maintenance):
  - `common.mk` expands `pdex.elf: $(OBJS)`'s prereqs at parse time,
    using a `$(SRC:.c=.o)` substitution that leaves `.cpp` extensions
    untouched. The override in our Makefile couldn't catch the prereq
    list because make had already baked it in. Two workarounds applied:
    1. We redefined `pdex.elf` with our own recipe that links
       `$(RV_OBJS)` (a properly substituted list). Make warns about
       duplicate recipes but uses the latest (ours).
    2. The original (stale) prereq list still contains
       `build/core/video_soft.cpp` — that prereq is satisfied with a
       no-op `cp` rule that stages the .cpp into the build dir. Our
       relinking rule ignores it.
  - Added a `%.o : %.cpp` pattern rule that calls
    `arm-none-eabi-g++ -fno-exceptions -fno-rtti`. The .cpp file
    avoids RTTI / exceptions / STL, so plain gcc-driven linking works
    without pulling in libstdc++.
- Verified the .pdx launches cleanly in the Playdate Simulator: ROM
  loads, CRC `133E9372` (Wario Land J) matches, simulator stays
  running. Visual output of the renderer not yet confirmed from CLI
  (requires looking at the simulator window); user will read the
  on-screen `%.1fms %lu cy` telemetry to capture the first
  interpreter+render baseline.
- **Next step:** read telemetry; if rendering looks plausible (title
  card or similar), add input mapping so we can press Start and watch
  the demo. If the screen is black/garbage, debug the soft renderer's
  XPEN gating (game may not have reached the state that enables the
  VIP pipeline yet — at which point only direct CPU framebuffer
  writes would be visible).

### 2026-05-28 — first picture: Wario Land precaution screen rendering

User screenshot from the Playdate Simulator showed Virtual Boy Wario
Land's bilingual "IMPORTANT: Read instruction and precaution booklets
before operating" disclaimer screen, rendered with legible English and
Japanese kanji/kana, centered 384×224 inside the Playdate's 400×240
display. First emulated frame to produce a visible picture.

End-to-end pipeline confirmed working:

- V810 interpreter executes past the boot vector into game code
  (`PC=0xFFFC2326`, ROM offset 0x1C2326 after VB's address mirroring).
- VIP state machine ticks: `XPCTRL & XPEN = 0x0002` ⇒ pixel pipeline
  enabled, all relevant interrupt bits pending (`INTPND = 0x401e =
  LFBEND | RFBEND | GAMESTART | FRAMESTART | XPEND`).
- Soft VIP renderer (`source/common/video_soft.cpp` linked from
  `playdate/core/`) composites BG worlds and OBJ data into
  V810_DISPLAY_RAM at the right offsets.
- `update_texture_cache_soft` populates the CHR-derived tileCache;
  text glyphs reconstruct correctly.
- `pd_video.c`'s 2bpp → 1bpp threshold (value ≥ 2 ⇒ white) preserves
  enough detail that the precaution screen text is readable.
- Centering (8 px / 8 px margins) is correct.
- 648 emulated VB frames at this point ⇒ approx 13 s of VB wall-clock
  time, 32 s of Playdate wall-clock time at 20 fps target.

**Telemetry read from the screenshot:** `2.5 ms / 420503 cy / r=0`.
**Critical caveat: this is on host Mac via the simulator dylib, NOT
on Cortex-M7 hardware.** The Mac is roughly 100× faster per cycle
than the Playdate ARM, and there is no SDRAM-stall penalty on the
host. The 2.5 ms means almost nothing for predicting real device
performance — it just confirms the architecture builds and runs
end-to-end without falling over.

The real benchmark — and the only number that matters — is ms/frame
**on actual Playdate hardware** (Cortex-M7 @ ~180 MHz, 16 KB D-cache
on top of 16 MB SDRAM). Beetle VB's postmortem says the same code
shape will be a SDRAM-stall fight; we expect a steep slow-down going
from simulator to device. Capture that next.

Also unresolved / to investigate:

- White-on-transparent text overlay (`kDrawModeFillWhite`) was needed
  because the original `kDrawModeCopy` rendered black-on-black over
  the (mostly black) VB output. Worth keeping the draw-mode toggle
  pattern in mind for any future on-game-screen UI.
- The precaution screen may persist longer than expected because the
  game's wait loop runs on its internal timer, which advances at
  the emulator's emulated speed, not Playdate wall clock. Need to
  watch whether `f=` keeps climbing and PC eventually leaves the
  `0xFFFCxxxx` ROM region.

- **Next step:** test input — confirm A / A+B (Start) / D-pad reach
  the game by watching `PC` change. Then run on **real Playdate
  hardware** and capture the first device-side ms/frame number.

### 2026-05-28 — FIRST REAL HARDWARE NUMBERS (the data the project exists for)

Side-loaded `RedViper.pdx` to the device (`pdutil <port> datadisk`, copy to
`/Games/`, eject, `pdutil <port> run /Games/RedViper.pdx`). Telemetry moved
off-screen to `logToConsole`, batched one line per 20 frames so logging
doesn't perturb the measurement. Captured on the Wario Land precaution
screen (the game's boot busywait — see caveat below).

**Initial device numbers (interpreter + naive blit):**
```
int=113 ms   vip=14 ms   blit=121 ms   tot=246 ms   (~4 fps)
```
- `int` (interpreter, V810 emulation): ~113 ms. Matches Beetle VB's
  pre-JIT interpreter (87–127 ms) almost exactly. Same CPU, same ROM,
  same SDRAM-stall problem.
- `blit` (my 2bpp→1bpp framebuffer convert): **121 ms — bigger than the
  interpreter.** Cause: the VB framebuffer is column-major (consecutive
  rows are 64 bytes apart), and the first blit scanned by output row, so
  every one of ~86k pixel reads missed cache and hit SDRAM.

**Fix #1 — column-major blit.** Rewrote `pd_video_blit` to iterate by VB
column (64 contiguous bytes = ~2 cache lines, reused for all reads in that
column) and scatter into the Playdate framebuffer (which lives in fast
cached RAM). Cache misses dropped from ~86k to ~768.
```
blit: 121 ms -> 1.2 ms     (~100x)
```
New total ~128 ms (~7.8 fps). **Render path is now solved**: vip+blit
together ≈ 15 ms, well inside any frame budget. Lesson reinforced from the
Beetle VB postmortem: on this hardware, memory *access pattern* dominates;
the same loop reordered is 100x faster with zero algorithmic change.

**Post-fix breakdown — the interpreter is now 88% of the frame:**
```
int=114 ms   vip=14 ms   blit=1.2 ms   tot=128 ms
```
~400000 emulated VB cycles/frame in ~114 ms ⇒ ~3.5 MHz effective vs the
VB's 20 MHz ⇒ we are ~5.7x too slow on CPU alone. To hit the 20 fps / 50 ms
"playable" target with ~15 ms of render, the interpreter must drop to
~35 ms (~3.2x speedup). To hit native 50 Hz it needs ~5.7x.

**Important caveat about this measurement point:** `PC` is pinned at
`0xFFFC2326`/`0xFFFC232A` — a 4-byte busywait loop. Wario Land spins the
CPU for the entire frame on the precaution screen. red-viper's *DRC* has
busywait detection that collapses these idle spins; the *interpreter*
(all we have on Playdate, DRC disabled) brute-forces every cycle. So this
is close to a worst case for idle screens, and NOT representative of
gameplay. The real benchmark must be captured in demo/gameplay mode.

**Fix #2 (in flight) — interpreter ROM-fetch fast path.** `interpreter.c`
fetched every instruction via `mem_rhword()`, which is in another TU (no
inlining), returns a `uint64_t` with wait-state bits packed in the high
word, and dispatches through a memory-region switch. Added a static-inline
`itrp_fetch()` that, when PC is in the ROM region (the overwhelming common
case), reads the 16-bit instruction straight from the ROM buffer and skips
the call / switch / wait-state math / uint64 packing. Falls back to
`mem_rhword` otherwise. Behaviour-identical (interpreter times from
`opcycle[]`, not the discarded wait bits). Inert on the 3DS+DRC build (the
interpreter never runs with PC in ROM there). Device measurement pending.

**Decision framing for the dynarec question:** the render path is no longer
a concern. The interpreter is the whole game now. Options, cheapest first:
1. Interpreter micro-opt (fetch fast path = fix #2; possibly decode-cache,
   busywait detection in the interpreter). Low effort, bounded upside —
   Beetle VB's interpreter tuning only bought ~15%.
2. Busywait detection ported from the DRC into the interpreter. Helps idle
   screens hugely, helps gameplay where the game waits on VIP/timer.
3. Thumb-2 dynarec backend (rewrite of `arm_emit`/`arm_codegen`/`drc_exec`
   for Cortex-M7). The real fix per the postmortem; multi-week effort.
Plan: exhaust cheap interpreter wins and measure each on-device before
committing to (3), so we know how much the dynarec actually has to buy.

**Fix #2 measured on device:** interpreter ROM-fetch fast path took the
interpreter from ~114 ms to ~83 ms (−27%) on the precaution screen — more
than Beetle VB's entire interpreter-tuning effort (~15%) from one change.
Total ~98 ms (~10 fps). Still measuring the busywait screen, so this is the
idle-loop figure, not gameplay.

### 2026-05-28 — Fix #3: busywait detection in the interpreter (built, measurement pending)

Ported the DRC's busywait concept into `interpreter.c`:
- `busywait_body_ok(target, branch)` walks the loop body and returns true
  only if every instruction is idempotent — loads (except pointer-chase
  `reg1==reg2`), MOV/MOVEA/MOVHI, AND/ANDI, CMP/CMP_I, `or rX,rX`,
  `add r0,rX`. Everything else (stores, ADD_I/SUB counters, jumps, nested
  branches) returns false. This is the same whitelist as
  `drc_findLastConditionalInst`, and it deliberately excludes counting
  loops (`add -1,rX; bne`) which must not be skipped.
- Verdicts cached per branch PC in a 256-entry direct-mapped table
  (`bw_cache`). Only ROM branches are classified (ROM can't self-modify,
  so a verdict is permanent).
- In the branch-taken path: if the branch is backward (`disp <= 0`), in
  ROM, and classified as a busywait, fast-forward exactly like the HALT
  handler — set `cycles = target`, zero `cycles_until_event`, call
  `serviceInt` in a loop until an interrupt moves PC or the frame ends,
  then return. This collapses an idle spin (potentially the whole ~400k-cy
  frame) into one event-advance step.
- Inert on the 3DS+DRC build: there the interpreter never runs with PC in
  ROM (the DRC owns ROM), so the `last_PC in ROM` gate is always false.

Expected effect on the precaution screen: it IS a busywait, so `int` should
drop dramatically (most of the 83 ms is the spin). The bigger question is
gameplay, where busywait detection recovers the idle time games spend
waiting on VIP/timer interrupts — exactly what makes the DRC fast on 3DS.

Build is ready; device dropped off USB before this build could be pushed
(serial port `/dev/cu.usbmodemPDU1_...` and the PLAYDATE disk both absent —
device asleep/locked or cable). **Next: push fix #3, read `int` on the
precaution screen, then get into gameplay (press A+B = Start, or wait) for
the representative number that actually decides the dynarec question.**

### 2026-05-28 — busywait measured + FIRST GAMEPLAY NUMBERS (decision-grade data)

Pushed fix #3 and the user drove through menus -> main menu -> demo level 1
-> demo level 2. This is the representative data the project needed.

**Busywait detection — confirmed working:**
- Precaution screen `int`: 83 ms -> ~20–32 ms. The spin is now fast-
  forwarded to each event instead of brute-forced. Input also confirmed
  working (PC walked through menu code and an interrupt vector at
  `0xFFFFFE10`; A/B/D-pad reach the game).

**Menus / simple scenes: PLAYABLE.**
- `int≈20–26  vip≈6–7  tot≈28–33 ms` ⇒ ~30 fps. Genuinely smooth.

**Demo gameplay — both costs balloon, and rendering is NOT solved:**
- Demo L1: `int≈42  vip≈54  blit≈2  tot≈98 ms` (~10 fps). **The soft VIP
  renderer (54 ms) is the BIGGER cost here — bigger than the interpreter.**
  On the simple precaution screen vip was only 14 ms; a full BG + sprite
  gameplay scene is ~4x heavier.
- Demo L2: `int≈60–92  vip≈25–37  tot≈80–175 ms` (~6–10 fps), spikier.
- Occasional `int` spikes to 120–147 ms on scene transitions (cold code /
  new tile sets — the SDRAM working-set effect from the Beetle VB
  postmortem, now visible in our interpreter too).

**Revised understanding (important):** the earlier "render is solved"
conclusion was an artefact of measuring a trivial screen. In real gameplay
the interpreter and the software VIP renderer are *co-equal* costs, each
~25–55 ms. Both must come down. Menus already are playable.

**Fix #4 — single-eye soft renderer.** `video_soft.cpp` composited BOTH
eyes (per-world loops ran `eye < 2`), but the Playdate display is 1-bit
mono and the blit only reads the left eye — half the raster work was
thrown away. Added `SOFT_EYE_COUNT` (1 on Playdate/Simulator, 2 on 3DS),
applied to all four eye loops (clear, normal-world, affine, object).
Expected ~2x on gameplay `vip` (~54 -> ~27 ms), demo L1 ~98 -> ~70 ms
(~14 fps). 3DS stereo unchanged. Measurement pending next gameplay run.

**Where the dynarec decision stands:** menus are already playable on the
pure interpreter + busywait detection. Gameplay needs both renderer
(single-eye now; more raster opt possible) and interpreter (40–90 ms,
needs ~2x for 20 fps) to improve. The interpreter half ultimately points
to a Thumb-2 dynarec — but the cheap wins already took us 246 ms -> ~30 ms
on menus and ~70–100 ms in gameplay, so simpler / "less demanding" games
look reachable on the interpreter alone. Decide after the single-eye
gameplay number lands.

### 2026-05-28 — single-eye measured; cheap-win phase complete

Single-eye render measured on device across menus + demo L1 + demo L2:
- `vip` in gameplay: ~54 ms -> **~19–25 ms** (better than 2x: whole
  right-eye-only worlds are skipped, and the FB clear is halved too).
- Menus: `int≈22 vip≈6 tot≈30 ms` (~30 fps) — playable.
- Demo L1 steady: `int≈38 vip≈19 tot≈58 ms` (~17 fps).
- Demo L2 steady: `int≈42 vip≈25 tot≈68 ms` (~15 fps).
- Scene transitions / heavy spots: `int` spikes 75–148 ms,
  `tot` 90–160 ms (6–11 fps).
- `blit` steady ~2 ms (negligible). `cy`≈400000/frame confirms one VB
  frame per Playdate tick.

**Cumulative result of the cheap-win phase (all on real hardware):**

| build | menus tot | demo gameplay tot |
|---|---|---|
| initial            | —    | 246 ms (4 fps) |
| blit fix           | —    | 128 ms |
| fetch fast-path    | —    | 98 ms |
| busywait detection | ~30 ms (30 fps) | 98 ms |
| single-eye render  | ~30 ms (30 fps) | 58–71 ms (14–17 fps) |

Render is now ~20 ms (down from a 121 ms naive blit + 54 ms both-eye
raster). The interpreter is the entire remaining problem: ~40 ms steady,
spiking to ~148 ms on transitions. The spikes are the Beetle VB
postmortem's exact failure mode — cold instruction fetch thrashing the
16 KB I-cache against the 2 MB ROM working set. A block-caching dynarec
(translate once, run hot from I-cache, inline memory access, chain blocks)
is precisely the fix for both the steady cost and the spikes.

## ASSESSMENT CONCLUSION (answers the project's central question)

**Can red-viper's architecture make VB emulation practical on Playdate?**

- **Architecture ports cleanly.** The V810 core, memory map, software VIP
  renderer, busywait detection, and timing model all compile and run on
  Cortex-M7 with only the platform frontend rewritten. The 3DS sources are
  reused near-verbatim via the `core/` symlinks; only ~5 small guarded
  changes to shared files.
- **red-viper's key speed techniques port and work:** busywait detection
  (ported from the DRC into the interpreter) is a large, real win;
  the dirty-cache software renderer works; single-eye + 1-bit threshold
  is a clean fit for the Playdate panel.
- **"Less demanding" content is already practical** on the pure
  interpreter: menus, static/simple scenes, and low-world-count games run
  at ~30 fps. A puzzle game or a homebrew title with modest VIP usage is
  plausibly playable today.
- **Mid-complexity gameplay (Wario Land) is ~15 fps** and not smooth on
  the interpreter. The renderer is no longer the issue; the V810
  interpreter is. red-viper hits full speed on 3DS *because of* its
  dynarec, which we cannot reuse directly: it emits ARM A32, and Cortex-M7
  is Thumb-2 only. A Thumb-2 backend is the remaining lever for full-speed
  gameplay.

**Difference from the Beetle VB outcome:** Beetle VB concluded the port was
not viable (28–70 ms with a JIT, never real-time). We reach a more
optimistic place: starting from a DRC-shaped emulator with busywait
detection and a dirty-cache renderer, the *interpreter alone* already makes
simple content playable, and the render path is solved. The same SDRAM
working-set wall is visible (the `int` transition spikes), but the
architecture has more headroom than Beetle VB's interpreter-first design.

## REMAINING OPTIONS (the task's open dynarec questions)

1. **Ship interpreter-only, scoped to less-demanding games.** Zero further
   core work. Add frame-skip + a game-compat list. Honest about what runs.
2. **More interpreter micro-opt** (decoded-block cache; WRAM/VRAM data-
   access fast paths like the ROM fetch path). Bounded upside — maybe
   15 fps -> ~20 fps in gameplay; will not reach 50 Hz.
3. **Thumb-2 dynarec backend.** The real fix. Largest effort (rewrite of
   `arm_emit`/`arm_codegen`/`drc_exec` for Thumb-2; needs an executable,
   writable code region on Playdate + I/D cache maintenance via CMSIS —
   `SCB_CleanDCache_by_Addr` / `SCB_InvalidateICache_by_Addr`). Open
   sub-questions still to derisk: can a Playdate app get RWX memory at all,
   and what's the per-block cache-flush cost? Keep the V810 decode/IR in
   `drc_core.c`; only the emitter + dispatch stub are new.

Recommendation: the assessment question is answered. Next concrete step is
the user's strategic call between (1)/(2)/(3) — captured in the session
log. Whichever path, the cheap-win phase is complete and well-measured.

## DYNAREC TRACK (user chose option 3: Thumb-2 backend)

### 2026-05-28 — step 1: RWX / cache-flush feasibility probe

Before writing any emitter, prove generated Thumb-2 can execute on device
and nail the cache-maintenance call. Probe generates `adds r0,#1; bx lr`,
flushes, calls fn(5), expects 6.

- **Attempt 1 (SCB registers): crashed.** Poking the Cortex-M7 SCB cache
  registers directly (`SCB_DCCMVAC` @ 0xE000EF68) bus-faults — Playdate
  apps run unprivileged. crashlog: `r1=e000ef68 cfsr=0x00000400`
  (BFSR IMPRECISERR) at `pc=0x600016e2`.
- **Two facts learned from the crash log:**
  1. SCB cache-maintenance registers are off-limits to app code; need a
     privilege-safe flush.
  2. The app itself executes from SDRAM (`pc=0x600xxxxx`), so a
     heap-allocated (SDRAM) code buffer is executable — execute-permission
     is NOT a blocker for the dynarec.
- **The SDK provides the right tool:** `pd->system->clearICache()` (SDK
  2.0+), a firmware, privilege-safe D-clean + I-invalidate. This is almost
  certainly how the Beetle VB JIT did it.
- **Attempt 2 (clearICache + heap buffer): measurement pending.** Allocate
  code via `pd->system->realloc`, write the two halfwords, `clearICache()`,
  call with Thumb bit set. If it returns 6, the dynarec is fully viable:
  heap = executable code cache, `clearICache()` = per-block flush.

**Cache-flush cost note for later:** `clearICache()` appears to be a full
I-cache clear (no by-range variant exposed), so calling it per translated
block could be expensive. Plan: translate eagerly / batch, flush once after
emitting a run of blocks, and minimise re-translation — exactly the
"larger code cache, fewer flushes" lesson from the Beetle VB postmortem.

### 2026-05-28 — step 1 result: RWX OK; step 2 result: EMITTER OK

- **RWX OK.** `pd_jit_test` (clearICache + heap buffer) returned 6 from
  generated `adds r0,#1; bx lr`. Generated Thumb-2 executes; the firmware
  `clearICache()` is the correct, privilege-safe flush. All dynarec
  blockers cleared.
- **Thumb-2 emitter (`src/t2_emit.h`) validated on device, 3/3.** Encoders
  for movw, movt, mov32 (movw+movt), add/sub/and/orr/eor (reg), cmp, mov
  (reg), bx/blx, push/pop, ldr/str (imm offset). Self-test emitted three
  functions (reg arithmetic; 32-bit immediate; ldr/sub/str memory) and all
  returned correct results. Encoding layer — the highest-risk part — is
  trustworthy.

## DYNAREC ARCHITECTURE DECISION

Two ways to build the Thumb-2 backend:

- **(B) Port the 3DS `drc_core.c` wholesale, swapping its A32 `arm_emit.h`/
  `arm_codegen.h` for Thumb-2 equivalents.** Reuses battle-tested block
  decode, register allocation (V810 regs in r4–r10), PSW↔CPSR flag mapping,
  busywait, branch opt. BUT the A32 emit layer leans on ARM-only mechanisms
  — conditional execution via predication (Thumb-2 needs IT blocks),
  `msr/mrs cpsr_f` in the `drc_exec.s` trampoline — so the port is fiddly,
  and `drc_core.c` is tangled with 3DS includes (citro3d/video_hard).
- **(A, CHOSEN) From-scratch minimal JIT.** Memory-backed V810 registers
  (load operands from `v810_state.P_REG[]` into ARM scratch, compute, store
  back — no register allocation yet), explicit flag computation in emitted
  code (mirror the interpreter's Z/S/OV/CY math rather than mapping to ARM
  CPSR), block = straight-line run until control flow or an unsupported op,
  interpreter fallback for everything not yet translated. Grows op coverage
  incrementally; every step verifiable on device.

Rationale for (A): correctness is incremental and testable; avoids the IT-
block / CPSR-trampoline complexity and the 3DS entanglement of (B). It will
be slower than the 3DS register-allocated DRC at first (memory-backed regs,
explicit flags), but it's a buildable path to *a* working JIT. Register
allocation and native-flag use are later optimisations once it's correct
and measured. If (A) plateaus below target, revisit (B) for the hot paths.

**First op set (no flags, lowest risk):** MOV, MOV_I, MOVEA, MOVHI, and
LD/ST/IN/OUT (loads/stores don't touch PSW). Control flow and any flag-
setting op end the block and return to the C dispatcher, which services
interrupts and runs unsupported ops via the interpreter, then resumes. This
proves translate→chain→dispatch→fallback end-to-end before arithmetic +
flag emission is added.

### 2026-05-28 — step 2b: code cache + block cache (CACHE OK)

`src/pd_jit.{h,c}`: heap code-cache buffer with a bump cursor and
`clearICache()` flush, plus an open-addressed V810-PC -> code-pointer block
map. Self-test (t4) emitted two distinct blocks, looked them up, ran both,
and confirmed an untranslated PC returns NULL. 3/3 CACHE OK on device.

### 2026-05-28 — step 3a: move-subset translator (XLATE OK)

`pd_jit_translate` walks a V810 instruction stream and emits Thumb-2 for
MOV / MOV_I / MOVEA / MOVHI, stopping at the first unsupported opcode.
Block ABI (so far): called as `void blk(uint32_t *p_reg)`, r0 = V810
register-file base, r1/r2 scratch; reads of r0 emit a literal 0 (matching
that the interpreter never reads P_REG[0]). Self-test (t5) translated a
4-move program, stopped correctly at an ADD, ran it against a mock register
file: r5=7, r6=7, r7=107, r8=0x12340000 all correct. 5/5 XLATE OK.

## DYNAREC SUBSTRATE: FULLY PROVEN ON HARDWARE

Everything that could have killed the approach is now validated on device:
- RWX (heap executable) + `clearICache()` flush — RWX OK
- Thumb-2 encoders (movw/movt/add/sub/and/orr/eor/cmp/ldr/str/bx/...) — EMITTER OK
- code cache + PC->block map + multi-block emit/exec — CACHE OK
- V810->Thumb-2 translation + execute + fallback boundary — XLATE OK

What remains is additive translation work plus the live integration. None of
it is high-uncertainty anymore; it's careful, individually-testable build-out.

## REMAINING ROADMAP TO A GAMEPLAY WIN

Important: none of the JIT affects the live `int` timing yet — the emulator
is still 100% interpreter. The `int` numbers are unchanged because the
dispatcher (step 3d) isn't wired in. A gameplay speedup needs the whole
chain below, because the dispatcher is all-or-nothing for the runtime.

- **3b — loads/stores.** Emit calls to the existing C mem helpers
  (`mem_rword`/`mem_wword`/...). Introduces the block prologue/epilogue
  (`push {r4,lr}; mov r4,r0` keep state in a callee-saved reg across the
  `blx`; `pop {r4,pc}`) and AAPCS arg setup. Self-testable in isolation.
- **3c — arithmetic + flags.** ADD/SUB/CMP/AND/OR/XOR/NOT/shifts with
  correct V810 PSW (Z/S/OV/CY). **Flag-handling design decision (made):**
  keep V810 PSW resident in the ARM condition flags (NZCV) across a block —
  ARM ADDS/SUBS/ANDS set N,Z,C,V which map to V810 S,Z,CY,OV — and only
  materialize PSW into the state struct at block exit or when an instruction
  reads it. This is the proven 3DS-DRC approach and the only way to real
  speed (per-op explicit flag packing would be correct but too slow). Needs
  a few new encoders (ADDS/SUBS/ANDS, MRS/MSR APSR, BIC/ORR imm). Each
  self-tested against the interpreter's exact flag results.
- **3d — branches + dispatcher.** Translate conditional/uncond branches and
  JMP/JR/JAL; build the C dispatcher that owns the cycle/event/interrupt
  model (mirroring `interpreter_run`'s target/serviceInt loop), calls
  translated blocks, and falls back to the interpreter for any
  not-yet-translated op. Riskiest integration (a cycle/interrupt bug faults
  mid-game); gated behind a flag so the interpreter build stays the safe
  default. **This is the step where `int` finally moves.**
- **4+ — optimisation.** Register allocation (map hot V810 regs to ARM
  r4–r10 like the 3DS DRC instead of memory-backed), block chaining (jump
  block→block without returning to the dispatcher), busywait in the JIT,
  wider op coverage (MUL/DIV/BSTR/FPP), code-cache eviction policy. This is
  where the JIT pulls meaningfully ahead of the interpreter.

Status: substrate complete; ~4 more build-out steps to a measurable
gameplay number, each individually testable on device. The hard unknowns
are behind us — what's left is volume and careful correctness.

## ITCM / DTCM EXPERIMENT (2026-05-28) — negative result, important

Per the user's Beetle VB experience (ITCM was their single best lever),
implemented the same technique for Red Viper:
- Forked `link_map.ld` to add an `.itcm.rv` section (`__itcm_rv_start/end`).
- `pd_itcm.c`: relocatable fast-path memory accessors (WRAM+ROM inline,
  indirect-pointer fallback to mem_* for other regions), marked
  `__attribute__((section(".itcm.rv")))`, built `-fno-jump-tables`.
- Per frame: memcpy the 448-byte `.itcm.rv` region into a 32-byte-aligned
  **stack buffer** (the Playdate stack is DTCM, zero-wait-state), `dsb`/`isb`,
  repoint `g_rv_*` into the copy. Interpreter data accesses route through
  `g_rv_*`. `relocate=1` confirmed on device.

**Result: no measurable change to `int`.** Benchmarking note: only the
**demo** segment is comparable across runs — it auto-plays with fixed length
and content. The earlier menu/"precaution" phase is a manual, button-driven
process that differs every run, so its numbers are NOT comparable build-to-
build (don't read anything into them). The conclusion here rests on the
fixed demo: **demo level 1 steady-state `int` was ~40 ms both before and
after ITCM** — identical. That is the reliable signal.

**Why it didn't transfer from Beetle VB:**
1. **Our accessors were already I-cache-hot.** They're tiny (448 bytes,
   called every instruction) and never get evicted, so pinning them in
   DTCM removes a miss that wasn't happening. Beetle VB's mednafen core
   funnels *all* memory through bigger bus callbacks (with banking logic)
   that competed for I-cache — pinning those paid off; ours don't.
2. **We'd already done the fetch fast-path.** `itrp_fetch` reads ROM
   directly (inlined), so the hottest per-instruction memory op never even
   calls an accessor. Little left for accessor-relocation to give.
3. **The real cost is elsewhere and TCM-proof.** Red Viper's interpreter
   cost is dominated by (a) the big `interpreter_run` dispatch loop (one
   large function with a jump-table switch — can't be cleanly relocated)
   and (b) **D-side SDRAM latency** reading the 2 MB ROM instruction stream
   and WRAM/ROM operands. ITCM only accelerates *instruction* fetch of the
   emulator's own code; the dominant data-side stalls on a 2 MB working set
   don't fit any TCM (DTCM is ~64 KB, ~7.7 KB usable after RTOS). This is
   the Beetle VB memory-latency wall, and TCM can't move it for an
   interpreter over a 2 MB ROM.

**Conclusion:** the ITCM/DTCM lever does not transfer to Red Viper's
interpreter — architectural difference (inlined fetch + monolithic dispatch
vs. callback-funnelled core) plus the un-TCM-able 2 MB working set. The
infrastructure (linker section, per-frame DTCM relocation, pointer routing)
is built and correct, and is kept because it has a natural home: **the JIT
code cache.** Generated blocks are small, hot, self-contained, and
relocatable — exactly the profile that benefits from zero-wait-state DTCM
execution, and exactly what the Beetle VB JIT's "cold 80 ms" cliff needed.
So the payoff for this work comes when the dynarec lands, with its code
cache in DTCM.

**Recommendation:** stop tuning the interpreter (it's at its floor: gameplay
~15 fps, menus ~30 fps). Resume the dynarec; put its code cache in DTCM.
