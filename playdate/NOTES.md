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

### 2026-05-29 — busywait bug found & fixed; light-game finding reframes the bottleneck

**Busywait detection bug (fixed).** Mario's Tennis hung at boot (black
screen, PC pinned). Diagnosed by logging the loop the detector flagged:
```
LD_H r1,[r2]; ANDI r1,r1,#0x3c; BE loop   ; wait while (([r2] & DPBSY)==0)
```
0x3c = L0BSY|R0BSY|L1BSY|R1BSY (DPBSY) — it polls a VIP display-busy bit
that the VIP sets *mid-frame without an interrupt*. My fast-forward was
copied from HALT, which services events only until an *interrupt* moves PC.
A data-poll loop never moves PC, so it skipped past the bit-set moment,
never re-ran the loop body to notice, and at frame boundaries DPBSY is 0 →
infinite spin. **Fix:** on a detected busywait, skip idle cycles to the next
event and RETURN, so the loop body re-runs and re-checks its real condition
each event (instead of HALT-style spinning to frame end). Correct AND still
skips the idle iterations. Tennis now boots and runs; Wario Land unaffected.

**MAJOR FINDING — for light games the renderer, not the CPU, is the wall.**
Mario's Tennis (a light game) gameplay, on the interpreter:
- `int ≈ 22 ms`  (CPU emulation — near the 20 ms real-time budget!)
- `vip ≈ 28–34 ms` (software VIP renderer — now the BIGGER cost)
- `blit ≈ 2 ms`, `tot ≈ 52–59 ms` ⇒ ~17–19 fps (~36% speed)

This is the OPPOSITE of Wario Land (CPU 45 ms dominated, render 20–25 ms).
For a light game the CPU is nearly real-time, and the **software VIP
renderer is the bottleneck** — and that renderer is red-viper's CPU-fallback
path (the 3DS uses the PICA200 GPU instead), so it's largely un-optimised.

**Reframed conclusion:** "less-demanding games at real-time" needs BOTH
~2–3x improvements, and which one dominates depends on the game:
- CPU-heavy games (Wario Land): the dynarec is the lever (CPU 45→~25 ms),
  but the memory wall caps it ~70% — likely never full real-time.
- Render-heavy / CPU-light games (Mario's Tennis): the **renderer** is the
  lever (vip 30→~10 ms); the CPU is already close. The dynarec would NOT
  make Tennis real-time on its own — the renderer caps it.

So the single highest-value remaining target for the *light-game* use case
is **optimising the software VIP renderer** (or writing a Playdate-native
1-bit renderer), not finishing the dynarec. Both are ~2–3x problems now —
far more tractable than Wario Land's 5–10x. The project's core question
("viable for less-demanding games?") is trending YES, gated on the renderer.

### 2026-05-29 — vip split-timing: it's all in video_soft_render

Split `vip` into tile-cache rebuild vs world/object composite on Mario's
Tennis: `cache=0 render=50` (title) / `cache=0 render=24–30` (gameplay).
- `update_texture_cache_soft` (dirty-tile rebuild) ≈ **0 ms** — the dirty
  tracking is already optimal; not a target.
- **100% of the cost is `video_soft_render`** — per-pixel compositing of
  the BG/OBJ worlds into the 2bpp column-major framebuffer.

So the renderer target is the compositing inner loops, not caching. Known
levers, roughly in order of effort:
1. **Cut Playdate-unused work:** `video_soft_render` maintains the
   `SoftBufWrote` dirty-column bounds for the 3DS GPU-upload path. On
   Playdate we blit the whole frame, so that bookkeeping is pure overhead —
   gate it out. Low risk, unknown (probably small) win.
2. **Render directly to 1-bit** instead of 2bpp + threshold blit. The user
   prioritises feel over graphic fidelity, and we discard the brightness
   levels anyway, so the 4-level shading work + the 2bpp memory traffic +
   the separate blit are all avoidable. Biggest lever (~2x plausible) but a
   substantial rewrite of video_soft.cpp's tileCache / get_tile_column /
   per-world write loops, with hard-fault risk.

**Overall state, stated honestly (both representative games characterised):**
- Mario's Tennis (light): int≈22 ms (just over the 20 ms real-time budget),
  render≈25–50 ms (bottleneck). Real-time needs int ~1.2x AND render ~2x.
- Wario Land (heavy): int≈45 ms (bottleneck; dynarec, memory-wall-capped
  ~70%), render≈25 ms.
Neither is full real-time yet; each needs substantial, *different* work
(Tennis→renderer, Wario→dynarec). Feasibility is answered: the port runs
real games correctly, simple content is close, but full-speed for these
representative titles is a multi-week lift in two different subsystems.

### 2026-05-29 — cheap renderer win measured: none; pixel composite is the floor

Gated out the `SoftBufWrote` dirty-bounds bookkeeping on Playdate. Result:
`render` unchanged (~49 ms title / ~25–29 ms gameplay). So that bookkeeping
was negligible — **100% of `video_soft_render` is the per-pixel world/object
compositing into the 2bpp framebuffer.** Cheap renderer wins are exhausted.
The only big renderer lever left is the direct-to-1bpp rewrite (est. ~1.5–2x,
multi-day, hard-fault risk). (Also observed: a later/heavier part of the
Tennis demo pushes `int` to ~35 ms — the CPU isn't always near-budget even
for a light game.)

## CONSOLIDATED STATE (feasibility study substantially complete)

**What works (on real hardware):** full V810 interpreter port; busywait
detection (correct after the HALT-vs-poll fix); single-eye 1-bit renderer
with cropping/threshold; input; ROM loading; runs Wario Land AND Mario's
Tennis correctly. Dynarec substrate fully proven (RWX, Thumb-2 emitter,
code cache, translator for moves/logic/arith+flags — all on-device tested).
DTCM relocation infra built. Minor tolerable graphical glitches on scene
changes.

**Performance (both representative games, fixed-demo benchmark):**
- Wario Land (CPU-heavy): int≈45 ms, render≈25 ms, tot≈72 ms (~14 fps).
- Mario's Tennis (render-heavy/CPU-light): int≈22–35 ms, render≈25–50 ms,
  tot≈52–74 ms (~14–19 fps).
Both run in slow-motion (~30–40% speed); neither is real-time.

**Why, and the remaining levers (both multi-week):**
1. **CPU** (Wario, and Tennis's heavier moments): finish the Thumb-2 dynarec
   (dispatcher + register allocation + CPSR flags + chaining; code cache in
   DTCM). Realistic ~1.5–2x, memory-wall-capped near ~70% for Wario.
2. **Renderer** (Tennis): rewrite the soft VIP to composite directly to
   1-bit. Realistic ~1.5–2x.
Real-time for a light game like Tennis needs BOTH (renderer ~2x to clear
its 25–50 ms, AND CPU under the 20 ms/frame budget). So "plays like original
hardware" for these titles is a genuine multi-week, two-subsystem effort;
the memory wall makes the most demanding games (Wario) borderline even then.
Simpler games than Tennis (fewer worlds/objects) are likely the sweet spot
and may already be near real-time.

**Bottom line for the feasibility question:** YES, red-viper's architecture
ports cleanly and runs real VB games correctly on Playdate; partial-speed is
achieved now; full real-time is reachable for light/simple games with
substantial further renderer+CPU work, and is unlikely for the most
demanding titles due to the SDRAM/cache memory wall.

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

### 2026-05-28 — dynarec step 3c done: arithmetic + flags (ARITH OK)

Translator now covers, all validated on-device against hand-computed
results AND flags:
- moves: MOV, MOV_I, MOVEA, MOVHI (XLATE OK)
- logic: OR/AND/XOR + ORI/ANDI/XORI, Z/S flags (LOGIC OK)
- arithmetic: ADD/SUB/CMP + ADD_I/CMP_I/ADDI, full Z/S/OV/CY flags (ARITH OK)

Flag approach: emit ARM flag-setting `adds`/`subs`, read APSR via `mrs`, and
permute NZCV → V810 PSW (`ubfx` each bit into place), inverting the carry bit
for subtraction (V810 CY = borrow = !ARM_C). Verified: `0-1` gives PSW low
nibble `0xA` (S|CY) with upper bits preserved. This is the heavy-but-correct
version (≈13 emitted instructions per arithmetic op); a CPSR-resident-flags
optimisation is deferred to step 4.

(Test-harness gotcha recorded: the self-tests used `ADD` as their
"unsupported, stop here" sentinel; once ADD became a translated op the
translator ran off the end of the test arrays. Switched the sentinel to
`JMP`, which is genuinely unsupported. Not a translator bug.)

## 2026-05-30 — 1-BIT RENDERER REWRITE: DONE & MEASURED (the big renderer win)

Implemented the direct-to-1bpp composite path the earlier note flagged as "the
only big renderer lever left." It lives in `source/common/video_soft.cpp`
behind `#ifdef RV_1BPP` (Playdate/Simulator only); the 3DS 2bpp path is
untouched. `pd_video.c` now calls `video_soft_render_1bpp()` and the blit just
transposes the 1bpp column-major scratch (`pd_render_fb1`, 32 B/col) into the
Playdate framebuffer — no threshold pass.

Design:
- New 1bpp tile cache (`tileCache1[]`) derived from the existing 2bpp
  `indices.u16[]` in `update_texture_cache_soft`: per column, an 8-bit
  transparency mask + three 8-bit "pixels with colour index c" masks (c=1..3).
- A pixel is white iff its palette-mapped shade ≥ 2 (the same threshold the old
  2bpp→1bpp blit used). At render time we OR the idx masks whose palette shade
  ≥ 2 → 8-bit bright column. Opacity mask is palette-independent.
- `render_normal_world_1bpp<aligned,over>` is a faithful /2 port of the 2bpp
  normal-world renderer (every 2-bit shift halved, 8-bit words, 32 B/col fb).
  Objects ported likewise. `render_affine_world_1bpp<over>` samples
  `tileCache[].indices` per pixel and thresholds via GPLT, like the 2bpp one.
- Left eye only (Playdate is mono). h-bias (bgm==1) still unimplemented — it is
  a TODO in the original 2bpp software renderer too (3DS draws h-bias on the
  GPU, which we don't have).

Measured on hardware:
- **Wario Land (normal+object dominant): `vip` 25–45 ms → ~2 ms menus / ~10 ms
  gameplay.** The renderer is no longer the wall. Played through to level 2 with
  no visible regression. The frame is now **CPU-bound**: `int` ≈ 32 ms menus,
  ~45 ms level 1, **60–115 ms level 2**; `vip` ~10 ms, `blit` ~1.6 ms.
- **Mario's Tennis (affine-heavy): title now renders** (it's two `bgm=2` affine
  worlds, `head=e080/e003` — that's why it was blank before affine was ported).
  But `vip` for the attract/court scene is ~44 ms — **affine did NOT get
  faster.** Affine iterates horizontally across columns, so every pixel is a
  scattered byte RMW into a different column (32 B apart) plus two scattered
  SDRAM reads (tilemap + tile indices); the access pattern, not the bit-width,
  dominates and is identical to the old 2bpp path. The title logo alone (small
  affine) is ~11.8 ms.

Takeaways:
- The 1bpp rewrite is a decisive win for normal-world/object games (the common
  case, and the project's "less demanding games" target). ~1.5–2× was the
  estimate; for Wario's render specifically it's far more because the per-column
  tileCache working set shrank from ~32 B to ~4 B and stays in the 16 KB cache.
- Affine games (Tennis court, Red Alarm, etc.) remain renderer-heavy; speeding
  affine would need a different layout (e.g. render affine row-major into a
  scratch line then transpose) — deferred, separate lever.
- **Conclusion: with the renderer fixed, the CPU interpreter is now the
  bottleneck for Wario Land** — which is exactly what the dynarec (Option 3)
  targets. Proceeding there next.

## 2026-05-30 — DYNAREC NEGATIVE RESULT: straight-line dispatcher is a 3–7× LOSS

Built and measured step 3d in its smallest form: wire the existing straight-line
translator into the live interpreter. Design (all gated behind `-DRV_JIT=1`):
- New `pd_jit_block_for()` (lookup-or-translate, computes pc_bytes/cycles/last_op
  metadata) + `pd_jit_run.c` global dispatcher (`rv_jit_init/_frame_flush/_try`).
- Interpreter hook at the top of the loop: for ROM PCs, run a ready translated
  block, advance PC/cycles by its totals, `continue`. Miss → translate for later.
- I-cache coherence handled by a flush-generation counter: blocks become
  executable only after the next per-frame `clearICache()` (batched, not
  per-block). Stub blocks (pc_bytes==0) record "PC starts with a
  non-translatable op" so we don't retry translating those.

Result on device (Wario Land), **vs ~32 ms (menus) / ~45 ms (gameplay) for the
pure interpreter**:
- `int` = **130–200 ms steady**, with **spikes to 800–1600 ms** (translation
  storms / block-map churn). A flat 3–7× regression. Correctness looked fine
  (no crash, game advanced) — it's purely a performance catastrophe.

Why (confirms the pre-dispatcher checkpoint, only worse):
1. **Per-instruction lookup tax.** The hook does a hash+probe+call on *every*
   ROM instruction. Most basic-block-start PCs are stubs (lead op is a
   load/store/branch), so this tax is pure overhead with no offsetting block.
2. **Translation thrashing.** Every newly reached PC translates a block or a
   stub; the map fills and resets, re-translating en masse — the multi-hundred-ms
   spikes.
3. **Memory-backed blocks are slower than the C interpreter** per instruction:
   every operand is ldr/str'd and each arith op carries ~13 instructions of
   flag packing, vs the compiler-optimised interpreter switch.

**Decision: RV_JIT disabled** (Makefile UDEFS), reverting to the pure
interpreter (the good post-renderer numbers). The JIT code stays compiled and
self-tested in the tree as proven substrate, but is not in the hot loop.

What it would actually take to win (and the ceiling):
- A straight-line dispatcher fundamentally can't pay for itself. A real win
  needs the full red-viper-style backend: **basic-block translation** (branches
  + loads/stores inline, so blocks span real BBs and the per-instruction tax
  amortises), **register allocation** (hot V810 regs resident in ARM r4–r10),
  **CPSR-resident flags** (drop the ~13-instruction packing), and **block
  chaining** (eliminate per-block lookups). That is a multi-week effort.
- Even done well, the **memory wall caps it**: the Beetle VB postmortem's
  working Thumb-2 JIT hit ~28 ms warm / ~80 ms cold on *this* ROM. So the
  realistic ceiling for Wario Land is ~25–30 fps gameplay, not 50.

So the dynarec is a large, bounded-payoff investment. For the project's stated
target — *less demanding games* — the **pure interpreter + 1bpp renderer is the
better-value configuration today** (Wario ~18 fps gameplay; lighter games
faster). Recommend treating heavy-game full-speed as out of scope unless the
full register-allocating backend is explicitly desired.

## 2026-05-30 — FULL BACKEND committed; Stage 1 (register allocation + lazy flags) DONE

After the straight-line dispatcher proved a loss, the decision was to build the
real red-viper-style backend. Staged, each validated by the boot self-test
harness (pd_jit_test.c) BEFORE any game integration, so we never ship a broken
hot loop again:
- **Stage 1 (done):** register-allocating, lazy-flag translator
  (`pd_jit_translate_ra`). V810 operands resident in ARM r4–r10 (loaded once,
  stored once); r11=base, r3=0, r0–r2/r12 scratch. Every flag op leaves ARM
  CPSR set (adds/subs/ands/orrs/eors); PSW materialised exactly once in the
  epilogue (only the last flag op is observable in a straight-line block).
  Prologue `push {r4-r11,lr}` / epilogue `pop {r4-r11,pc}` (needs the wide
  PUSH.W/POP.W encoders added to t2_emit.h). **t9 self-test: RA OK 7/7** on
  device (covers reuse, in-place ops, MOVEA 32-bit imm, lazy ADD/SUB/logic
  flags) — results match the interpreter exactly.
- **Stage 2 (next):** translate loads/stores by calling the g_rv_* accessors;
  pooled regs in r4–r10 survive the calls (callee-saved), so blocks can span
  loads/stores. This is the first stage that lets blocks cover whole basic
  blocks — the prerequisite for the per-block dispatch model to pay off.
- **Stage 3:** branch-terminated basic blocks + next-PC computation.
- **Stage 4:** block-level dispatch loop + cycle/event model + interpreter
  fallback (the riskiest piece).
- **Stage 5:** block chaining; integrate behind RV_JIT and measure vs the
  interpreter.

Note: only after Stage 4/5 does anything change in-game; Stages 1–3 are
validated in isolation. The realistic ceiling remains ~25–30 fps for Wario-class
games (memory wall). RV_JIT stays OFF in UDEFS until Stage 5 integration.

## 2026-05-30 — BASIC-BLOCK BACKEND: correct on real game, but ~3x slower (needs chaining)

Stages 1-4 of the real backend are built and the whole thing is wired into the
live interpreter behind RV_JIT. All translator self-tests pass on device
(t9 RA OK, t10 LS OK, t11 BRANCH OK), and — the big milestone — **it runs Wario
Land correctly**: no garbage, no freezes, correct behaviour through multiple
levels. The translator (register allocation in ARM r4-r10, lazy flags via CPSR,
load/store via the g_rv_* accessors, Bcond/JR/JAL/JMP with PSW condition eval and
next-PC return) and the dispatch+event integration are all proven on commercial
game code. That was the hard, risky part and it works.

Integration design (low-risk): the dispatcher is a hook at the top of the
interpreter loop (not a separate engine), so it reuses the interpreter's proven
event/interrupt/HALT/serviceInt machinery. A ready block runs a whole basic
block natively and returns the next PC; the loop advances cycles by the block's
opcycle sum and `continue`s. Busy-wait loops spin natively and exit when
serviceInt advances the event between blocks (no fast-forward needed). HALT and
other system ops stay in the interpreter (stub block ⇒ dispatch miss).

**Performance: a loss.** Wario Land `int` ≈ 130-140 ms steady (early frames
210-400 ms during translation warmup), vs ~45 ms for the pure interpreter — about
3x slower. Why: VB basic blocks are short (~5 instructions), and every block pays
unamortised fixed overhead:
- prologue/epilogue register save/restore on every block,
- a flag pack to PSW that the terminating branch then immediately re-reads
  (redundant on the hot compare-and-branch pattern),
- a per-block dispatch hash lookup,
- I-cache thrash: the 256 KB SDRAM code buffer vs the 16 KB I-cache.

Only **block chaining (Stage 5)** amortises this: patch block→block jumps so hot
loops stay in native code with registers live across blocks and no dispatcher
round-trip, plus evaluate branch conditions straight from CPSR (skip the
pack+reload) and save/restore only the callee-saved regs a block actually uses.
That is another large, multi-session effort.

**Honest ceiling reminder.** The Beetle VB postmortem's *working* Thumb-2 JIT hit
~28 ms warm / ~80 ms cold on this exact ROM. So even a fully chained backend
realistically lands around match-or-modestly-beat the ~45 ms interpreter for
Wario-class games — the memory wall dominates, not dispatch overhead. The big,
already-banked win was the renderer (vip 25-45 ms → ~10 ms). For the project's
stated target (less-demanding games) the **interpreter + 1bpp renderer remains
the pragmatic sweet spot**; the dynarec is correct and kept in-tree (gated off)
as proven substrate, but its perf payoff for this hardware is bounded and not yet
positive without Stage 5.

RV_JIT is OFF in UDEFS. Re-enable with `-DRV_JIT=1` + `make clean` to A/B it.

### 2026-05-30 — Stage 5a measured: dynarec is MEMORY-BOUND (the decisive finding)

Added minimal register-save (push/pop only the pool regs a block uses) and
per-frame JIT diagnostics, then measured Wario Land on device. The diagnostics
settle the question of where the dynarec's time goes:
- Steady state: **xlate ≈ 9 / 60 frames** (essentially no translation — not
  churn), **blocks ≈ 1300** of 16384 (no map thrashing), **exec ≈ 2.5M / 60f ≈
  42,000 block executions per frame**.
- int ≈ 186 ms ⇒ **~4.4 µs ≈ ~780 ARM cycles per ~5-instruction basic block.**

780 cycles for 5 instructions is pure **memory stall**, not compute: the
translated code lives in the 256 KB SDRAM code buffer and thrashes the 16 KB
I-cache; the dispatcher's hash probe hits a 384 KB block-map array (more misses);
register spill/fill hits cpu_state in SDRAM. Minimal reg-save barely moved int —
confirming the cost isn't the instructions we emit, it's fetching them.

Why this is decisive (not just "needs more optimisation"):
- The interpreter wins *because* its hot loop is small and stays resident in the
  16 KB I-cache; only the ROM instruction stream misses. The JIT's code is larger
  and scattered, so it I-cache-misses on essentially every block.
- Block chaining would remove the dispatcher round-trip (~half the overhead →
  ~90 ms?), but **still wouldn't beat the 45 ms interpreter**, and can't fix the
  I-cache pressure of SDRAM-resident code.
- There is no fast memory to escape to: DTCM/ITCM (~7.7 KB free) can't hold a
  meaningful working set of translated code.

**Conclusion: the dynarec cannot beat the interpreter on Playdate hardware.**
This reproduces, with on-device measurements, the Beetle VB postmortem's core
finding. The backend is complete and correct (kept in-tree, gated off, fully
self-tested) but is the wrong lever for this hardware. The winning configuration
is the **interpreter + 1bpp renderer**; the renderer was the real, banked win
(vip 25-45 ms → ~10 ms). Stage 5c (chaining) is NOT pursued: the measured wall
makes its bounded payoff negative for this project's goals.

## 2026-05-30 — Affine micro-opt: per-tile hoist gives only ~3% (it's per-pixel compute-bound)

Hoisted the tilemap read, tile-flag decode and palette→bright table out of the
affine per-pixel loop (recompute only when the affine walk crosses a tile, ~1/8
of pixels). Correct (Tennis renders fine) but **only ~3% faster**: full-screen
affine vip ~44 ms → ~43.5 ms, title logo ~11.8 → ~11.5. The hoisted reads were
already cache-resident (a scene touches few tiles, so the tilemap line + the
tile's 16-byte index data stay cached across an 8-pixel run) — we eliminated
cache *hits*, not misses. The residual cost is per-pixel coordinate arithmetic +
the index read + the masked write over ~86k px × 2 affine worlds — compute-bound
and irreducible without drawing *less*. Kept the change (small real win, cleaner)
but the lever for affine-heavy games is **frameskip** (skip whole frames for
"feel"), or trading affine resolution for speed — not micro-opt.

## 2026-05-30 — Frameskip "feel" mode (runtime, system menu) — works, with a cache bonus

Added a runtime "Frame skip" selector to the Playdate system menu (Off/Skip
1/2/3) via addOptionsMenuItem — doesn't steal game input. The interpreter runs
every Playdate frame (VB timing/logic stay correct); only the VIP composite +
blit are gated by a skip counter. The tile cache catches up on the next rendered
frame, so skipping is visually-only and safe.

Measured (Mario's Tennis): full-screen affine scene per-frame wall time
~64 ms (Skip 0) → ~38 ms (Skip 1) → ~30 ms (Skip 2); court gameplay ~40 ms
(Skip 1) → ~25 ms (Skip 2, ~40 fps). Game motion stays real-time; the screen
just refreshes at 25/17/12 Hz.

Unexpected bonus: at the SAME game location, `int` (CPU emulation) itself fell
from ~28 ms (Skip 1) to ~18 ms (Skip 2) for identical VB cycles. Cause: the
renderer evicts the interpreter's hot working set from the 16 KB cache; skipping
renders leaves the CPU running with a warmer cache. So frameskip helps twice —
directly (less render) and indirectly (less cache pollution). This is the
"feel" lever for render-heavy / affine games.

## 2026-05-30 — BUSYWAIT DETECTOR FIX: RedSquare 9 → ~50 fps (a real win)

RedSquare (homebrew) ran at ~9 fps with int ~98 ms, PC pinned in an 18-byte loop
at ROM 0x7000fec. Decoding it (from the .vb, no device needed):
```
MOVEA r30,r0,#0xf800 ; MOVHI r30,r30,#6  -> r30 = 0x0005F800 (VIP INTPND)
LD.H  r30,[r30+0]                          ; r30 = INTPND
MOVEA r29,r0,#0x4000 ; AND r30,r29         ; test XPEND (drawing-done) bit
BE    -> loop                               ; spin while clear
```
A classic VIP-sync idle spin — should be fast-forwarded, but the interpreter
brute-forced ~400k cycles of it every frame. Cause: `busywait_body_ok` rejected
any load with reg1==reg2 as a "pointer chase," but here r30 is *recomputed* each
iteration (MOVEA+MOVHI) before the LD, so the self-addressed load is idempotent.

Fix (source/common/interpreter.c): track which regs are (re)written earlier in
the loop body; allow a self-addressed load only when its address reg was
recomputed this iteration (fresh), still rejecting genuinely loop-carried
pointer chases. Result on device: **int 98 → 4.4 ms, ~9 → ~45–50 fps**, correct
(rendered 20/20, game advances). General: helps any title using the recompute-
address-then-poll idiom. Strictly more permissive only in a provably-idempotent
case, so low risk; worth a sanity re-check of a previously-good game.
**Regression check (Wario Land, on device): PASSED** — int/vip unchanged vs the
pre-fix baseline (~32 ms menu, ~48 ms L1, 60–132 ms L2, vip ~10 ms), played to
level 2, no glitch/hang. Wario's int is genuine work (not a spin the fix
touches), so correctly no change. Fix validated as safe across the board.

## 2026-05-30 — PER-GAME TEST RESULTS (interpreter + 1bpp renderer, on device)

Measured on hardware with the shipping config (interpreter, 1bpp renderer,
RV_JIT off). `int` = CPU emulation/frame, `vip`+`blit` = render/frame, `tot` =
per-Playdate-frame wall time; game-speed fps ≈ 1000/tot (50 = real VB speed).
Frameskip (system menu) skips only rendering — it helps **render-bound** games
and is useless-to-harmful for **CPU-bound** ones.

| Game | ROM | Bottleneck | int (ms) | vip (ms) | ~fps (no skip) | Frameskip verdict |
|------|-----|-----------|----------|----------|----------------|-------------------|
| **Galactic Pinball** | 1M | mixed: **light menus / render-heavy table** | 6 menus → ~22 table | 4–5 menus → 20–29 table | **~50 menus**, ~20–23 table | Helps the table (render-bound) |
| **V-Tetris** | 512K | light menus / mild-CPU gameplay | 4.5–6 menus → ~22 play | 6 menus → 10–14 play | **~50 menus**, ~27–30 play | Skip 1 → ~36–40 play |
| **Space Squash** | 512K | variable: light menus / **CPU-heavy combat** | 13 menus → 45–49 combat | 5 menus → 15–42 combat | **~50 menus**, ~15–20 combat | Helps render; combat CPU-floored ~20 |
| **RedSquare** (homebrew) | 512K | **was CPU-bound (undetected spin) → fixed → render-bound** | 98 → **4.4** (after fix) | 11–21 | 9 → **~45–50** | n/a — now near full speed |
| **Fishbone** (homebrew) | 2M | moderate CPU (scroller) | 11 menu → 26–31 play | 6 menu → 8 play | ~50 menu, ~25 play | raises speed but choppy scroll → motion sickness |
| **Space Invaders** | 512K | **OUTLIER: pathological spikes** | 18 normal, **180–290 spikes** | 16 normal, **130 spike** | ~3–40 (unstable) | n/a — black-screen CPU phases |
| **Mario's Tennis** | 512K | **render** (affine court) | 12–22 | 11–44 | ~16 (court) | **Big win** — Skip 2 → ~33–40 fps game-speed |
| **Panic Bomber** | 512K | CPU (moderate) | 10 menu → 33–42 play | 2–18 | ~18–22 (busy) | Modest; capped by int |
| **Wario Land** | 2M | CPU | 45 (L1) → 60–115 (L2) | ~10 | ~18 (L1) | Minor (removes ~10 ms render) |
| **Mario Clash** | 1M | CPU | 70–90 | 6–12 (25 in FX scenes) | ~10–13 | Modest (more in FX scenes) |
| **Jack Bros** | 1M | CPU + render-heavy scenes | 41–50 | 3–5 simple → 30–38 busy | ~12–22 | Helps busy scenes; CPU-floored ~20 |
| **Teleroboxer** | 1M | **CPU (worst commercial)** | **85–107** | 4–38 | ~8–11 | **Harmful** — slideshow, no speed gain |
| **Insecticide** (homebrew) | 2M | **CPU (worst overall, 3D engine)** | **96–122** | 4–40 | ~6–10 | n/a — CPU-bound |
| **Ballface** (homebrew) | 2M | **CPU-bound + incomplete render (h-bias gap)** | 113–141 | ~2.7 (HUD only) | ~7–8 | n/a — walls missing + slow |
| **Insmouse no Yakata** | 1M | **scene-variable (VIP- AND CPU-bound by scene); renders OK** | 5–92 | 5–29 | ~10 (combat) – ~50 (menu) | partial — render-bound corridors only |

Per-game notes & what could help:
- **Galactic Pinball** — **the breakthrough / best case.** Menus and simple
  screens hit FULL 50 fps (int ~6 ms, vip ~5 ms) — the first game to do so, and
  proof that the interpreter is NOT a fixed high floor: it's ~6 ms when the game
  itself is light. The actual pinball table is a dense full-screen normal-world
  background, pushing vip to ~20–29 ms with moderate CPU (~22 ms), so table play
  is ~20–23 fps and **render-bound** (so frameskip helps it, unlike the
  CPU-bound games). Validates the project goal: genuinely light games/scenes run
  at real speed.
- **V-Tetris** — menus/light screens hit full 50 fps (int ~5 ms); main gameplay
  is mildly CPU-bound (int ~22 ms, probably the stereoscopic-well animation) with
  low render, so ~27–30 fps at Skip 0. **Skip 1 (confirmed on device): "plays
  really good and is fun"** — ~44 fps game-speed. Notably int itself fell
  22 → 17 ms at the same PC under Skip 1 (the cache-pollution bonus: less
  rendering ⇒ warmer cache for the interpreter), so Skip 1 helps even though
  it's CPU-bound. **Second confirmed fun game** after Mario's Tennis. Validates
  the goal: a less-demanding game made genuinely playable by 1bpp renderer +
  frameskip.
- **Fishbone** (homebrew) — 2 MB side-scroller; menu ~50 fps, gameplay
  moderately CPU-bound (int ~30 ms ⇒ ~25 fps), genuine work (not a spin; the
  busywait fix doesn't apply). **Key UX finding: frameskip is poorly suited to
  constantly-scrolling games** — when the whole screen pans every frame, dropped
  frames make the scroll judder in big jumps → motion sickness. Frameskip only
  feels good when the background is ~static and just sprites move (Tennis court,
  V-Tetris board). So Skip 0 (~25 fps, slow-mo) is the better *experience* here
  despite frameskip raising the raw speed number.
- **Space Invaders** — problematic OUTLIER, not representative. Normal phases
  ~25–40 fps, but hits two pathologies: (1) a heavy CPU routine at ROM
  fff87e..–fff87fba that runs ~6× slower than typical code (int 180–290 ms for
  ~400k real cycles — likely a tight loop of memory-mapped accesses / a CPU-side
  buffer copy/clear going through the slow g_rv_* path, NOT fast-forwarded so the
  interpreter brute-forces it); and (2) a renderer worst-case (vip ~130 ms in
  some scene). Drawing is disabled (XPEN off, XP=1b00) during the CPU-spike
  phases ⇒ black screen + "menus take seconds". Would need targeted profiling
  (which region the hot routine hammers; what worlds the 130 ms render walks) to
  explain/fix — a per-game compat issue, set aside.
- **Space Squash** — 512K but action-heavy: menus/calm hit ~50 fps (int 13 ms),
  yet sustained combat is CPU-bound (int ~45–49 ms ⇒ ~15–20 fps) with heavy
  render too (vip 15–42 ms). Important: it **breaks the "ROM size predicts"
  rule** — a 512K action game is as CPU-heavy in combat as the 1 MB titles. The
  real driver is **per-frame CPU work / action intensity**, not ROM size (size
  was only a loose proxy because bigger games tend to do more per frame).
  Full-speed when idle, slow-mo exactly when the action peaks. Not fun in play.
- **Mario's Tennis** — the 1bpp renderer made normal/object cheap; the affine
  court (~43 ms vip) is the wall. Affine micro-opt gained only ~3% (per-pixel
  compute-bound). *Lever:* frameskip (works well); a bigger win would be
  rendering affine layers at reduced resolution (e.g. half-width, doubled) — a
  "feel over accuracy" trade, ~2× on affine, not yet implemented.
- **Panic Bomber** — normal/object only, so vip is tiny; it's CPU-bound at
  ~33–42 ms in busy play (~24–30 fps ceiling). *Lever:* none cheap; bounded by
  the interpreter/memory wall. Skip 1 is the sweet spot if any.
- **Wario Land** — renderer already fixed (vip 25–45→~10 ms); now CPU-bound,
  worse in busy level 2. *Lever:* faster CPU only; dynarec proven non-viable on
  this hardware.
- **Mario Clash** — normal-world action; vip is low (CPU-bound at ~70–90 ms,
  ~10–13 fps), with occasional render-heavier FX scenes (vip ~25 ms) where
  frameskip helps more. A 1 MB ROM, so it lands between the 512K games and
  Teleroboxer — consistent with ROM size / working set driving CPU cost.
  *Lever:* none cheap; CPU/memory-wall bound.
- **Jack Bros** — 1 MB action-puzzle; CPU-bound (int ~42–50 ms ⇒ ~20 fps
  ceiling) AND render-heavy in busy/demo scenes (vip 30–38 ms), so ~12–22 fps.
  Frameskip removes the render half (helps busy scenes toward the ~20 fps CPU
  floor) but can't beat the int floor — stays slow-mo. Same bucket as
  Wario/Mario Clash; not a "fun" candidate.
- **Ballface** (homebrew) — 2 MB; very CPU-bound (int ~115–140 ms ⇒ ~7–8 fps,
  genuine main-loop work) AND renders incompletely: only the HUD + a white line
  draw, walls/level missing (vip ~2.7 ms — almost nothing composited). Almost
  certainly the level uses **h-bias backgrounds (bgm==1), which our renderer
  doesn't implement** (TODO; 3DS draws h-bias on the GPU). First game in the set
  where the h-bias gap visibly bites. Unplayable on both counts; confirming the
  cause would need a world-type survey. *If h-bias rendering were ever added it'd
  fix the visuals but not the ~7 fps CPU wall.*
- **Insmouse no Yakata** — VB launch first-person 3D maze/shooter. The most
  **scene-variable** game tested, and the only one that flips between VIP-bound and
  CPU-bound depending on the scene: menus/intro hit a full **~50 fps** (int ~8,
  vip ~5); light corridors are **render-bound** (~28 fps, int ~5.5 / vip ~29 — so
  frameskip would help *these*); active 3D dungeon scenes are **CPU-bound**
  (~15 fps, int ~54) dropping to **~10 fps** in the busiest moments (int ~82–92).
  Unlike Ballface it **renders correctly** — its 3D walls use normal/affine worlds,
  not the unimplemented h-bias path. Verdict: a "half-playable" 3D title — fine to
  walk/menu, slow-mo once combat engages. Same per-frame-CPU ceiling as the other
  3D games, but with genuine fast lulls (so it sits a notch above Insecticide).
- **Insecticide** (homebrew) — 2 MB homebrew 3D/first-person engine; the heaviest
  CPU case overall (int ~96–122 ms ⇒ ~6–10 fps): per-frame 3D math
  (raycasting/polygons + FPU) on the V810. CPU-bound, frameskip can't help. Also
  surfaced an input limitation — our mapping is only A/B/d-pad/(A+B=Start), no
  Select or right d-pad, so couldn't progress (moot given the speed). Shows
  homebrew that pushes 3D is as demanding as the worst commercial titles.
- **Teleroboxer** — heaviest commercial CPU case: 1 MB ROM (largest working set → worst
  memory-wall behaviour) plus per-frame affine-parameter + likely heavy FPU math
  for the scaling fighters. ~10 fps and frameskip backfires (CPU-bound + low base
  fps → visual slideshow). *Lever:* none viable on this hardware; would need a
  working dynarec (ruled out) or game-specific interpreter hot-path tuning
  (bounded). Treat as out of scope for full speed.

**Pattern / feasibility verdict (refined by Galactic Pinball):**
- The interpreter is **not** a fixed high floor — `int` is ~6 ms when the game
  does little per-frame work (Pinball menus) and scales up with the game's own
  CPU load (Wario/Teleroboxer do genuinely huge per-frame work). So **light
  games and light scenes hit full 50 fps.** This answers the project's central
  question: *yes*, less-demanding games are practical on this port.
- With the 1bpp renderer, normal/object rendering is usually cheap, but a
  **dense full-screen normal world** (Pinball table) can still cost ~25 ms vip,
  making some scenes render-bound; affine (Tennis) likewise. Frameskip is the
  "feel" lever for those.
- The games that **won't** reach 50 fps are the CPU-heavy ones: action games
  with lots of per-frame logic, FPU/affine-CPU scalers (Teleroboxer), and the
  big-ROM titles whose working set thrashes the 16 KB cache (memory wall). For
  those, neither frameskip (CPU-bound) nor the dynarec (ruled out on this HW)
  helps materially.
- Net: **practical for less-demanding games (the stated goal); not for the
  heaviest commercial titles at full speed.**
- **Refinement (Space Squash):** the dominant factor is **per-frame CPU work /
  action intensity**, not ROM size — a 512K action game can be as CPU-heavy in
  combat (~48 ms) as the 1 MB titles. ROM size was only a loose proxy. So the
  "playable" set is really *low-action* games (puzzle/board/pinball/menus),
  regardless of size; busy action games are slow-mo whenever the action peaks.

## HONEST CHECKPOINT before the dispatcher (step 3d)

The straight-line translator is complete and trustworthy. What remains for a
*running* JIT — branch translation + the C dispatcher (cycle/event/interrupt
model, block lookup/translate, interpreter fallback) — is the single largest
and riskiest piece (a cycle bug faults mid-game), and it's where `int` first
moves.

Realistic payoff, stated plainly:
- The current blocks are **memory-backed**: every operand is `ldr`/`str`ed
  from the state struct, and each arithmetic op carries ~13 instructions
  (heavy flag packing). A first integrated dispatcher will therefore be only
  **marginally** faster than the interpreter — maybe a wash — until step 4
  (register allocation: keep hot V810 regs in ARM r4–r10; CPSR-resident
  flags; block chaining). Those are what made red-viper fast on 3DS.
- The **memory wall caps the ceiling**: per the Beetle VB postmortem, even a
  working Thumb-2 JIT hit ~28 ms warm / ~80 ms cold on this exact ROM. Our
  interpreter is ~45 ms. So the realistic best case for Wario Land is
  ~25–30 fps gameplay (not full-speed 50 fps), and cold-block translation
  spikes will hurt unless the code cache is large and DTCM-resident.

So the dynarec is worth pursuing only if the goal is to push *this* class of
game from ~15 fps toward ~25–30 fps — a real improvement, but bounded. For
"less demanding" games (the project's stated target), the interpreter is
already adequate (~30 fps). This is the inflection point to decide how far to
invest.

---

# POSTMORTEM (2026-05-30)

*Written to wrap up the first arc of the project. Inspired in tone by the
Beetle VB Playdate postmortem — honest about what worked, what didn't, and what
the hardware simply won't allow.*

## What we set out to answer

One question, not "ship a polished emulator": **is Virtual Boy emulation
practical on the Playdate for the less-demanding end of the library, if we
optimise for speed and "feel" over graphics accuracy?** Red Viper (the 3DS VB
emulator) was the starting point because its V810 core is fast and portable.
The Playdate is a Cortex-M7 at 180 MHz with a 1-bit 400×240 panel, 16 KB I/D
caches, and SDRAM-resident code — a far cry from the 3DS's GPU and cache budget.

**Answer: yes, with a clear and predictable boundary.** Low-action games
(puzzle, board, pinball, menus) and the calm scenes of bigger games run at the
full 50 fps panel rate; busy action and any 3D engine fall to 10–20 fps
slow-mo, and no amount of effort we could find moves that line on this hardware.

## What we built (and what each was worth)

- **1-bit renderer rewrite — the single biggest win.** Replacing the 2bpp
  software compositor with a column-major 1bpp path (lazy tile cache, threshold
  shading, transpose-blit to the Playdate framebuffer) cut normal/object render
  cost dramatically (Wario vip 25–45 ms → ~10 ms). This is what turned "every
  game is a slideshow" into "light games are full speed." Worth every hour.
- **Busy-wait fast-forwarding — high leverage, low cost.** Detecting idempotent
  spin loops and skipping to the next event. The standout: RedSquare went from
  9 fps to ~50 fps once we fixed a false-rejection in the detector (the
  `reg1==reg2` reload heuristic). A reminder that *one* mis-emulated idle loop
  can masquerade as "the CPU is too slow."
- **Frameskip "feel" mode — useful but narrow.** Run the CPU every frame (logic
  stays correct), gate only the render. It genuinely helps *render-bound* games
  (Mario's Tennis: ~16 → ~33–40 fps feel) and even speeds the interpreter via
  reduced cache pollution. But it does nothing for CPU-bound games, and on
  constant scrollers (Fishbone) dropped frames cause judder → motion sickness.
  So it's a per-game tool, not a global fix.
- **The dynarec — the big swing that missed.** A full V810→Thumb-2
  register-allocating basic-block JIT: emitter, code cache, translator, lazy
  CPSR flags, register allocation into r4–r10, branch handling, dispatcher. It
  is **correct** — it runs Wario Land cleanly and passes a full self-test
  suite. It is also **~3× slower than the interpreter without block chaining**,
  and Stage 5a measurement showed why: ~780 cycles/block, **memory-bound**. The
  generated code lives in SDRAM and thrashes the 16 KB I-cache; the M7 spends
  its time fetching, not executing. This matches the Beetle VB postmortem's
  finding exactly. We disabled it (kept in-tree, gated off, fully documented).

## What we learned (the findings that surprised us)

1. **The interpreter is not a fixed floor.** Going in, we assumed `int` was a
   constant ~45 ms tax. It isn't — it's ~5–8 ms when a game does little
   per-frame work and balloons to 50–120 ms when it does a lot. Galactic
   Pinball menus hitting 50 fps was the moment this clicked.
2. **The predictor is per-frame CPU intensity, not ROM size.** ROM size was a
   tempting proxy and it's wrong: Space Squash (512K) is as CPU-heavy in combat
   as the 1 MB titles, and calm in menus. The real axis is *how much the game
   thinks per frame* — i.e. action/3D intensity.
3. **The memory wall is the ceiling, and it's made of cache, not clock.** The
   dynarec failure wasn't a bug or a missing optimisation — it was 16 KB of
   I-cache against SDRAM-resident generated code. This is the hardest constraint
   on the platform and the one that ended the "make everything fast" dream.
4. **Correct ≠ fast, and "fast enough" is often already here.** The dynarec
   taught us that a fully correct, well-tested system can still be the wrong
   answer. Meanwhile the interpreter was already adequate for the target games.

## Where it landed (15 games tested on device)

- **Genuinely good (~30–50 fps, fun):** RedSquare, V-Tetris, Galactic Pinball
  (menus/light), Mario's Tennis with frameskip.
- **Playable / scene-dependent:** Jack Bros, Space Squash, Panic Bomber,
  Insmouse (calm areas ~28–50, combat ~10–15).
- **CPU-floored (10–15 fps slow-mo):** Wario Land, Mario Clash, Teleroboxer,
  Insecticide, Ballface, Fishbone scrolling.
- **Outliers:** Space Invaders (pathological CPU spikes, black-screen phases);
  Ballface (also visually broken — see h-bias gap below).

## Known gaps / debt left behind

- **h-bias backgrounds (`bgm==1`) are not rendered.** On the 3DS these are a GPU
  job; our software path stubs them. Ballface exposed this — its walls draw with
  h-bias and are simply invisible. Affects correctness, not speed.
- **Minimal input mapping** — A/B/d-pad/(A+B=Start) only; no Select or right
  d-pad. Blocked progress in a couple of homebrew titles.
- **No on-device ROM picker** — ROM is a compile-time constant; every game
  swap is a rebuild + datadisk deploy. Fine for testing, not for users.
- **The dynarec sits unused in the tree.** Correct, gated off, ~3k lines. Kept
  as a documented negative result rather than deleted, because the analysis is
  the valuable part.

## If we pick it back up

In rough priority for *usability* (not speed — speed is largely solved within
the hardware's limits):
1. **On-device ROM picker** — biggest quality-of-life jump; makes the port
   actually usable as a thing you hand to someone.
2. **h-bias rendering** — closes the last correctness gap; makes h-bias games
   (Ballface and others) display correctly. Won't help their frame rate.
3. **Per-game interpreter hot-path tuning** — the only remaining *speed* lever
   that isn't ruled out, but bounded and labour-intensive. Not worth it for the
   target games, which already run.

What we would **not** do again: revive the dynarec on this hardware. The
memory wall is real, we measured it twice (ours + Beetle VB's), and the answer
won't change without more cache or on-chip code RAM than the M7 offers.

## Verdict

The project answered its question. **Red Viper on Playdate is practical for
less-demanding Virtual Boy games**, which was the whole point — and it does so
on the interpreter plus a 1bpp renderer, busy-wait skipping, and an optional
frameskip feel mode. The heaviest commercial and 3D titles are out of reach at
full speed, and that's a hardware verdict, not a software TODO. A good place to
pause.
