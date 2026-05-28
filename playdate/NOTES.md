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
- **Next step:** read the on-screen ms/frame number now that it
  actually renders (telemetry-only screen) and commit it as the
  interpreter-only baseline. Then renderer + input.
