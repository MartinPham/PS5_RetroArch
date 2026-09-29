# Active work

_Updated: 2026-09-28_

## Now: v0.5.0-alpha.5 released, the first on RADV; Dolphin's first start next

**Released.** Build `f96bdb0e` (release notes `docs/releases/v0.5.0-alpha.5.md`):
the title links ../PS5_Vulkan's RADV release archive by default (PS5_Mesa
`cedb774`, built by ../PS5_Vulkan `5d8f37d`), SDK fork `95c08f2`, LRPS2 `6d14775` with
its port patch, the same tree as the fork's `main` at `9c2eea4` (6x). `PS5_VULKAN_DRIVER=ps5vk` still builds ps5vk,
which v0.4.0-alpha.4 (build `870bd1bb`) shipped. On RADV the release battery
(every core with a game, the menu, Close Content and a reload, Threaded Video on
half) passed three times as the driver changed, and PPSSPP's ten-minute soak once
(docs/PHASE_LOG.md, 2026-09-28). PPSSPP's MSAA works, RADV's shader cache is in
`radv-shader-cache/`, and the title heap has an arena for each thread. The
console's installed title is still v0.4.0-alpha.4's build.

**Dolphin round (after v0.4.0-alpha.4, on ps5vk):** the PS5 defaults are 6x, Async
(UberShaders), 16x AF and GPU texture decoding (patches/dolphin, e905c4e): RE4,
Melee, Wind Waker and Mario Kart Wii at 100% after boot. ../PS5_Vulkan R94
compiles pipelines on six threads at once; R95 writes tiled uploads a run at a
time. Rogue Leader still dips (attract 71-87%, gameplay 93-96%): Dolphin's MMU
emulation and its threads waiting on each other, not the driver.

**Seven more systems (2026-09-28, docs/PHASE_LOG.md):** Beetle PSX HW (16x),
Mupen64Plus-Next (ParaLLEl-RDP 8x), Beetle Saturn (needs a BIOS), VICE x64sc,
MAME 0.289 (vector screens at 4K, raster native), DeSmuME (5x) and Azahar (18x),
each from my fork at a pinned revision. They all pass the battery, and every
tested game runs at full speed. Load Content lists INTERNAL and EXTERNAL
(patch 0097). The installed title is build `4be9e814`.

Still open for them: EXTERNAL (`/mnt`) is empty inside the title's sandbox
(USB and extended storage need the title jailbroken; etaHEN's jailbreak call is
one way), Saturn gameplay with a BIOS and a vector game at 4K in MAME. The
forks are published on github.com/mihawk-99 at the pinned revisions, so a
checkout without the sibling forks builds from there.

**Now: RPCS3 as a libretro core** (docs/RPCS3_PORT.md). The goal is God of
War HD (NPUA80490, a PSN package plus its RAP) booting from `content/PS3/`
at a 300% scale on RADV, with the firmware from `system/RPCS3/`. Ladder steps
1 to 5 are done (docs/PHASE_LOG.md, 2026-09-28): the core builds from my fork
PS5_RPCS3 (`main`, published) with the pinned LLVM, FFmpeg and libiconv, loads
on the console, installs the firmware, and runs RPCS3's `gs_gcm_hello_world`
test with the LLVM recompilers on RADV, picture and full-speed audio, exiting
cleanly. Next is step 6: the game's package install and boot at 100%, then
300%, the battery and the ten-minute run. Known costs: the shader
interpreter's 6,650 pipelines compile before a first boot (about 100 s, then
cached), with no loading screen yet.

**Dolphin transitions (paused 2026-09-28 for RPCS3):** Rogue Leader's dips are
not shader compiles. The emulation thread spins in `sched_yield` inside a
libkernel lock for about half its slow time, and runs the MMU slow path for
the rest (docs/PHASE_LOG.md). Next: name that lock with the sampler's stack
scan, then compile-thread priority, the boot INI save, the GPU-thread cost and
the MMU fast path.

**Open:**

1. Dolphin's first start of a game on RADV, from an empty shader cache: Wind
   Waker's first two 10 s windows at 83% and 92%, where ps5vk had 86% and 99%.
   The time is Mesa's NIR optimisation of Dolphin's ubershaders
   (../PS5_Vulkan/docs/RADV_PHASE.md).
2. Rogue Leader in Dolphin (measured on ps5vk; not yet on RADV): I chose to make Dolphin itself faster on the PS5 --
   its MMU slow path, where 16 KiB host pages limit page-table fastmem, and its
   GPU-thread cost -- rather than underclock the emulated CPU (75% reached
   83-100% but changes the game's timing). Queued while the driver's Vulkan 1.4
   plan is decided (../PS5_Vulkan/docs/VULKAN_1_4_PLAN.md).
3. The CPU's transfers on the GPU (CP DMA for linear copies, vk_meta for tiled
   ones), with the golden comparisons updated for the new submission shapes.
4. The 60 Hz fallback on a display that stays at 60 Hz (the tester's trace).
5. LRPS2: the upstream PCSX2 port in ../PS5_LRPS2's working tree, 8x as the PS5
   default, the lighting compared with the native picture.

**Test runs and core options.** RetroArch here uses per-core options
(`global_core_options = false` in my config), so a run's `core_options_path`
is ignored and each core reads `config/<core>/<core>.opt`. A test that needs
its own options sets `global_core_options = "true"` with its own path; my
per-core files are never edited.

## Accepted baselines

- Native byte order on driver `8b311e3`: I accepted the colours with the red/blue
  compensation gone (`evidence/driver-1.0-native-byte-order/`).
- XMB: large-list allocation failures fixed (`601e575`), crash-free navigation
  (`docs/XMB_LIST_SAFETY.md`, `evidence/xmb-safe-list-run/`).
- Gameplay: Genesis Plus GX colours, sound, controls and transitions accepted;
  FCEUmm, mGBA, Snes9x and FBNeo evidence in `native-core-loading/`, `mgba-native/`,
  `snes9x-native/`, `fbneo-native/`.
- Thumbnail and configuration-reset fixes (patches 0081/0082).
- PPSSPP (God of War: Ghost of Sparta, Yu-Gi-Oh! GX Tag Force at 10x): picture,
  full speed at 120 Hz, save states, fast-forward, close and reopen.

## Named errors and remaining limits

- First frontend snapshot has five intentional missing-core recovery errors and two
  archive-extraction failures; I identified the content as Sega System 16 & 32,
  which belongs to FBNeo. No general archive fix is inferred.
- Console verification does not cover every supported system, BIOS/disc/CHD, NTSC and
  interlace options, SRAM/state round trips, long-run A/V or performance. Zero backend
  errors is not proof of uninterrupted audio across all workloads.
- Upstream compiler warnings (old zlib prototypes, Tremor's long-to-int abs conversion,
  an unused blip helper's missing return) are neither suppressed nor treated as
  acceptance.
- The loader still rejects TLS, legacy init/fini, nonempty preinit and unsupported
  relocations/dependencies; cores share the frontend process, so a rejected load is safe
  but an arbitrary core fault is not isolated.
- The withdrawn path has the console history; the native byte order does not yet. A
  colour regression would show as red and blue exchanged in the menu, in core frames or
  in thumbnails, and is the first thing to check on the next run.

## Preserved platform and file locations

XMB stays default and RGUI selectable. Vulkan remains the presentation route; video_ps5
stays selectable as CPU fallback. Native audio/input/binding, config saving, FTP
permissions and directory browsing remain. Deployments preserve live settings/games.

FTP base `/data/homebrew/PPSA99169/` corresponds to in-title `/app0`. Use content/,
system/, cores/, config/retroarch.cfg, savefiles/, savestates/. Genesis Plus GX Sega CD
BIOS files use system/ root; FBNeo uses system/fbneo/. See `docs/DEPLOYMENT.md`.

## Operating notes

Uploads are authorized; check console idle before deployment. A launch without args.txt needs my
intervention. Start klog first, preserve old logs, and capture allocation, frontend and
trace logs. Never interrupt another title. No new core or unrelated driver work is
assigned by this file.
