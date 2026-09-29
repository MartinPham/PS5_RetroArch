# RPCS3 as a libretro core: the port

This document is the audit behind porting RPCS3 (PlayStation 3) to the PS5
title as `rpcs3_libretro.so`, and the plan it leads to. Progress and run
results are recorded in `docs/ACTIVE.md` and `docs/PHASE_LOG.md`, not here.

## The objective

On my console, `rpcs3_libretro.so` boots God of War HD (the PSN release
NPUA80490) through RetroArch's Load Content or `/app0/args.txt`. The game
reaches gameplay, rendered by RPCS3's Vulkan renderer on RADV at a 300%
resolution scale (3840×2160 internal for its 720p output). The evidence is:

- a RetroArch screenshot of gameplay whose picture I accept;
- ten minutes of play with no crash;
- the Close Content + reload battery passing;
- each 10 s audio window's speed, recorded. Full speed is not part of the
  objective, but its measurement is.

What the console holds (read in place, never copied anywhere):

| Item | Where on the console |
| --- | --- |
| PS3 firmware | `/app0/system/RPCS3/PS3UPDAT.PUP` (206 MB) |
| The game | `/app0/content/PS3/God of War.pkg` (6.3 GB), a PSN package |
| Its licence | `/app0/content/PS3/UP9000-NPUA80490_00-GODOFWARHDUS0000.rap` |

Because the game is a package and not a disc folder, the first content path is
"install the package and its RAP, then boot the installed game". The
installed copy needs another 6.3 GB on the console's storage.

## The fork and the pin

`../PS5_RPCS3` is my fork of RPCS3/rpcs3 (github.com/mihawk-99/PS5_RPCS3,
public), with an `upstream` remote. Its `main` starts at upstream
**1707d7fc883e** (2026-09-28, "remove unnecessary #ifdef"): the newest master
commit whose CI passed all thirteen checks, including RPCS3's FreeBSD build.
The PS5's system software is FreeBSD-based, so that is the configuration
closest to this port. Every port change is a commit on the fork's `main`, and
`tools/build-rpcs3.sh` pins the fork by revision. Submodules RPCS3 names by
relative URL (`../../llvm/llvm-project.git` and the rest) resolve from the fork
as from upstream. A submodule that needs changes gets its own fork,
`../PS5_<Name>`, referenced by relative URL, as PS5_Azahar does with
PS5_Dynarmic.

## Licence

RPCS3 is GPLv2. It ships as a separate libretro core, loaded by RetroArch at
run time, on the same model as the Snes9x and FBNeo cores the title already
ships, and the fork's source is published at the pinned revision. The title
bundles no PS3 firmware, keys or games.

## Phase 1: the audit

### 1. Dependencies

RPCS3 builds its emulator as the static library `rpcs3_emu`, and its Qt
application links that. Upstream already builds `rpcs3_emu` without Qt for its
Android port (`ANDROID` in `CMakeLists.txt`): Qt, the Qt UI and the OpenGL
renderer are left out, OpenAL is compiled out (`WITHOUT_OPENAL`) and the
bundled FFmpeg is not used. The libretro
build follows that path, with a `LIBRETRO` switch of its own where Android's
choices differ.

`3rdparty/` at the pin (submodule revisions in brackets):

| Item | What RPCS3 uses it for | Decision |
| --- | --- | --- |
| llvm (llvm-project `ca7933e4`, LLVM 22.1) | PPU and SPU recompilers | **Build**: a pinned static LLVM for `x86_64-sie-ps5`, X86 target only (below) |
| asmjit (`416f7356`) | SPU ASMJIT recompiler, trampolines, JIT helpers | **Build**, its code memory through the platform layer (gap a) |
| ffmpeg (RPCS3/ffmpeg-core `649d9d93`, prebuilt) | video and audio decoding (cellVdec, cellAdec, ATRAC3+, cellMusic) | **Build from source**: a pinned FFmpeg release, static, software decoders (below); the prebuilt archive has no PS5 build |
| glslang (`f0bd0257`) | GLSL to SPIR-V for the Vulkan renderer's shaders | **Build** |
| GPUOpen/VulkanMemoryAllocator (`1d8f600f`) | Vulkan memory | **Build** (header library) |
| zlib (`da607da7`), zstd (`f8745da6`) | archives, save states, caches | **Build** |
| libpng (`3061454d`), stblib (`013ac3be`), bcdec | images, texture decoding | **Build** (bcdec and stb are headers) |
| yaml-cpp (`51a5d623`), pugixml (`c8033ce9`) | configuration, patches, XML | **Build** |
| SoundTouch (`a0fba77b`) | audio time-stretch in the resampler | **Build** |
| fusion (`93254240`) | motion-sensor fusion in `PadHandler.cpp` | **Build** (small C library) |
| unordered_dense | hash maps | **Build** (header) |
| protobuf (`edaa823d`) | RPCN (NP) messages | **Stub**: RPCN is compiled into `rpcs3_emu` unconditionally, so a `LIBRETRO` option leaves the NP network clients out with an offline implementation. Networking is dropped |
| wolfssl (`3c5eead4`), curl (`01346829`) | RPCN and clans over TLS | **Drop**, with the network clients (above) |
| miniupnp (`d66872e3`) | UPnP port mapping | **Drop**, with `upnp_handler` stubbed |
| libusb (`87a55632`) | USB passthrough (`sys_usbd`, `usb_device`) | **Stub**: no PS5 backend; `sys_usbd` reports no devices |
| hidapi | HID pads, only in `rpcs3/Input` | **Drop** (the libretro pad handler replaces `rpcs3/Input`) |
| rtmidi (`1e5b4992`) | MIDI instruments (Rock Band) | **Build** with RtMidi's dummy API, or stub the MIDI devices |
| OpenAL | microphone capture in cellMic | **Drop**: `WITHOUT_OPENAL`, as on Android |
| cubeb, FAudio, XAudio2 | audio output backends | **Drop**: a libretro audio backend replaces them |
| libsdl-org/SDL | input, camera, FAudio's dependency | **Drop** |
| opencv | PS Move tracking in `rpcs3/Input` | **Drop** |
| discord-rpc, feralinteractive (GameMode), pine | Discord, GameMode, PINE IPC | **Drop** |
| 7zip | not used by `rpcs3_emu` at the pin | **Drop** |
| MoltenVK, GL | macOS Vulkan, OpenGL headers | **Drop** (no OpenGL renderer, as on Android) |
| Qt 6 | the UI | **Drop** |

CMake options (top-level `CMakeLists.txt`):

| Option | Default | Port |
| --- | --- | --- |
| `USE_NATIVE_INSTRUCTIONS` | ON | OFF; `-march=znver2` instead, the PS5's Zen 2 (AVX2, FMA, BMI2), set in the toolchain file |
| `WITH_LLVM` | ON | ON |
| `BUILD_LLVM` | OFF | OFF: LLVM is built separately and found through `LLVM_DIR` |
| `STATIC_LINK_LLVM` | OFF | ON |
| `USE_FAUDIO`, `USE_SDL`, `USE_LIBEVDEV`, `USE_DISCORD_RPC`, `USE_GAMEMODE` | ON/OFF | OFF |
| `USE_VULKAN` | ON | ON |
| `USE_PRECOMPILED_HEADERS` | OFF | OFF (ccache) |
| `USE_SYSTEM_*` | mixed | OFF throughout: every library is built for the PS5 or dropped |
| `HAS_MEMORY_BREAKPOINTS`, `BUILD_RPCS3_TESTS`, `RUN_RPCS3_TESTS` | OFF | OFF |
| `USE_LTO` | ON | OFF: a 1 GiB-class shared object links in time without it; revisit when it runs |

**LLVM.** RPCS3 requires LLVM 18 or newer and builds 22.1 from its submodule.
I build that same revision (`ca7933e4`) once, as a pinned static install
(`tools/build-llvm.sh` into `.deps/native/llvm-ps5`), with `prospero-clang`,
ccache, `LLVM_TARGETS_TO_BUILD=X86`, and no tools, tests, docs or examples, and
point RPCS3 at it with `LLVM_DIR`. Keeping it out of RPCS3's build tree means a
change to RPCS3 never rebuilds LLVM. LLVM's own `Support` layer assumes Unix
facilities (`mmap`, `sysconf`, `dladdr`, signals) that the platform layer and
the core loader's import table provide or refuse; the link shows which. The
archive size, and what it adds to the core, are measured at step 3; LLVM with
only the X86 target is on the order of a few hundred MB of archives, of which
the core links the code generator, MCJIT and RuntimeDyld.

**FFmpeg.** RPCS3's prebuilt `ffmpeg-core` has no build for this console, so I
build FFmpeg from a pinned source release: static, no programs, no network, no
hardware acceleration. The decoders are the ones the PS3's media libraries
need (H.264, MPEG-2, MPEG-4 Part 2, ATRAC3, ATRAC3+, AAC, MP3, AC-3, LPCM), with
libavformat's MP4, MPEG-PS and raw demuxers, libswscale and libswresample. No
encoders: video recording is dropped. Software decoding comes first. The
console's hardware video decoder, which ProsperoLight uses, is a later option
for H.264.

### 2. The frontend

RPCS3 drives `Emu` (the `Emulator` in `rpcs3/Emu/System.h`) from a frontend
that fills `g_emu_callbacks` (`rpcs3/Emu/emu_callbacks.h`: 49 `std::function`
hooks) and calls `Emu.Init()`. `headless_application` shows the minimum: it is
a `QCoreApplication` whose callbacks create null renderers and handlers, and
it refuses Vulkan. The libretro layer, `rpcs3/libretro/` in the fork, replaces
it without Qt:

- **Entry points.** `retro_init` sets RPCS3's directories (below) and fills the
  callbacks. `retro_load_game` installs the firmware if it is missing, installs
  a package if the content is one, then boots. `retro_run` runs the queued
  main-thread calls (`call_from_main_thread`, which RPCS3 uses for UI-side work
  on the frontend's thread), presents the newest frame and pushes audio.
  `retro_unload_game` stops `Emu` and waits for its threads.
- **Vulkan.** RPCS3 creates its own instance (API 1.2) and device
  (`vkutils/instance.cpp`, `vkutils/device.cpp`). Through RetroArch's
  context-negotiation interface (v2) the core creates the instance at the
  version RPCS3 asks for (`create_instance`), and the device with RPCS3's
  feature and extension choices (`create_device2`). RetroArch uses that
  instance and device. RetroArch's own instance is 1.1, which is why the core
  creates it. RPCS3's existing `native_swapchain_base` owns its presentable
  images. A `swapchain_libretro` on top of it presents by handing the finished
  image to RetroArch with `set_image` and `set_signal_semaphore`, in the layout
  RetroArch expects. Every `vkQueueSubmit` and present that RPCS3's RSX thread
  makes on the queue it shares with RetroArch goes between the interface's
  `lock_queue` and `unlock_queue`.
- **Audio.** A `LibretroBackend : AudioBackend` takes cellAudio's output
  through the backend's write callback, resamples it to RetroArch's rate
  (48 kHz; RPCS3's audio is 48 kHz already, so resampling is a copy until a
  game asks otherwise), converts it to 16-bit stereo and queues it. `retro_run`
  hands it over with the batch callback.
- **Input.** A libretro pad handler (a `PadHandlerBase`) maps the RetroPad to
  a DualShock 3: buttons, both sticks and the analogue triggers. Pressure
  sensitivity comes from the digital state. Keyboard, mouse, camera and music
  handlers are the null ones.
- **Firmware.** The Qt application installs `PS3UPDAT.PUP` in
  `main_window::HandlePupInstallation` (about 320 lines). The libretro layer
  does the same work without Qt: `pup_object` (`Loader/PUP`), the update
  packages' TAR extraction and the SPRX decryption. It runs on first boot when
  `utils::get_firmware_version()` is empty and reports progress through the
  RetroArch message interface.
- **Content.** The formats, in order:
  1. A PSN package plus its RAP. This is the game at hand. The layer installs
     it with `package_reader` (`Crypto/unpkg`), as
     `main_window::HandlePackageInstallation` does (about 380 lines), and
     copies the RAP to `dev_hdd0/home/<user>/exdata/`
     (`main_window::InstallFileInExData`), then boots the installed
     `dev_hdd0/game/<title>/USRDIR/EBOOT.BIN`. An already installed package is
     not installed again.
  2. A disc game's folder (`PS3_GAME/USRDIR/EBOOT.BIN`).
  3. An ISO, which the pinned revision supports (`Loader/ISO.cpp`).
- **Where RPCS3 keeps its files.** RPCS3's emulator directory (`dev_flash`,
  `dev_hdd0`, config, caches) is `/app0/system/RPCS3/`, next to the firmware.
  Folders are 0777 and files at least 0666, so everything stays reachable over
  FTP.
- **Core options and PS5 defaults.** Resolution scale 300%. Shader mode
  asynchronous with the shader interpreter (`async_with_interpreter`, already
  upstream's default), so no game waits for a shader compile or relies on a
  warm cache. PPU and SPU decoders LLVM (upstream's defaults). Options are the
  libretro v2 kind, written to RPCS3's configuration when content loads
  (Phase 2 below).
- **Core info.** libretro-core-info has no RPCS3 file, so
  `tooling/rpcs3/rpcs3_libretro.info` is in this repository (name, extensions
  `pkg|iso|bin|elf|self|sfo`, firmware entry, GPLv2, `needs_fullpath`).
- **Reference ports.** Dolphin's `Source/Core/DolphinLibretro/` (the Vulkan
  negotiation, its instance and device hooks and main-thread handling),
  PPSSPP's libretro port and Azahar's.

### 3. Platform gaps

Each gap is either already measured on the console (`docs/PROBE.md`, the
platform layer's probes) or gets a probe step in the platform layer
(`src/probe.c`, host tests in `tests/test_platform.c`), run inside the title
with `/app0/platform-probe.txt`.

**(a) Code memory for LLVM and asmjit.** RPCS3 reserves code areas with
`utils::memory_reserve(size, can_be_jit=true)` and commits them
read-write-execute (`protection::wx`). Measured: direct memory mapped
read-write and then given execute runs, read-write-execute included, and a
protection change costs about 26 µs. `ps5_exec_allocate` has had no limit on
live blocks since SDK d3e8625: 2,000 4 KiB blocks and 300 64 KiB regions ran on
the console. LLVM uses the Small code model with PIC, so its code reaches
RPCS3's helpers through RuntimeDyld stubs and needs no placement near the
core. asmjit commits its 2 GiB area up front only under `CAN_OVERCOMMIT`
(Linux), which the PS5 branch leaves undefined, so it grows on demand.
*To measure:* a probe step that reserves a 2 GiB range, commits 64 KiB pieces
read-write-execute on demand, and runs code in each, with the time a commit
takes.

**(b) Address-space reservations.** RPCS3 reserves its guest memory in 4 GiB
steps from `0x3_0000_0000` upward (`Emu/Memory/vm.cpp`):

| Area | Size |
| --- | --- |
| `g_base_addr` + `g_sudo_addr` | 8 GiB |
| `g_exec_addr` | 12 GiB |
| `g_hook_addr` | 32 GiB |
| `g_stat_addr` | 4 GiB (`g_free_addr`, for SPU, starts right after it) |

That is 56 GiB, plus JIT areas (2 GiB for asmjit, 768 MiB for each LLVM
memory manager) and RSX's mappings. Measured (`docs/PROBE.md`): the GPU window
is `0x2_0000_0000`–`0x2_FFFF_FFFF`; from `0x4_0000_0000` at most 15 GiB; from
`0x10_0000_0000` to `0x80_0000_0000` 16 GiB is reserved at the hint itself;
nothing is granted at or above 1 TiB; and a hint whose free space is too small
is refused, not moved. The PS5 branch of `memory_reserve` therefore starts at
`0x10_0000_0000`, through `ps5_vrange_reserve`, which never draws on the
title's 448 MiB flexible pool, where an anonymous `mmap` reservation would.
*To measure:* one probe step reserving RPCS3's layout, 56 GiB in the sizes
above from `0x10_0000_0000`, to confirm 16 GiB is not a per-reservation cap
for 32 GiB.

**(c) Guest memory as views.** RPCS3's `utils::shm` creates a memory object
(`memfd_create`, or `shm_open(SHM_ANON)` on FreeBSD) and maps it at several
addresses: `g_base_addr` and `g_sudo_addr` see the same memory at different
protections. The PS5 branch uses `ps5platform/shm.h`: `ps5_shm_create` (one
direct-memory object), `ps5_shm_map` at fixed addresses inside a
`ps5_vrange_reserve` range, and `ps5_memfd_create` where RPCS3 wants a
descriptor. The platform tests already cover several views of one object.
*To measure:* 4 GiB of direct memory as two views, written through one and
read through the other, and the time a 64 KiB view takes to map.

**(d) 16 KiB host pages.** RPCS3 already handles hosts whose pages exceed
4 KiB (Apple Silicon): `vm.cpp` stops protecting 4 KiB guest blocks one by one
when `utils::get_page_size() > 4096` and maps them read-write, and every
`mprotect` is rounded to the host page. The RSX texture cache still protects
pages to catch CPU writes, now 16 KiB at a time, so a write near a cached
texture invalidates a wider range. *To measure:* what `sysconf(_SC_PAGESIZE)`
returns in the title (it must be 16384; the platform layer answers it
otherwise), and the texture cache's protection changes per frame in the game,
from RPCS3's own counters, at 26 µs each.

**(e) `thread_local` on emulated TLS.** The core loader has no PT_TLS support,
so a core's `thread_local` goes through `__emutls_get_address`. RPCS3 has 93
`thread_local` uses. The hot ones are `cpu_thread::g_tls_this_thread`,
`vm::g_tls_locked` and atomic.cpp's wait callbacks, all read on every guest
reservation and wait. *To measure:* the cost of an emulated-TLS read on the
console (a probe step timing `__emutls_get_address` over a million reads),
then RPCS3's rate of those reads in the game. If it matters, the fork keeps
those few variables in a per-thread structure the thread already carries.

**(f) Atomic waits.** On FreeBSD, RPCS3's `atomic.cpp` uses its `USE_STD`
path (`std::condition_variable`), and `Utilities/sync.h` emulates `futex()`
off Linux, so it needs only pthread mutexes and condition variables, which the
console exports. `_umtx_op` is exported too, if a faster path is wanted later.
Nothing to add.

**(g) Faults and `ucontext`.** RPCS3's access-violation handler
(`Utilities/Thread.cpp`) reads `uc_mcontext.mc_rip`, `mc_err` and the general
registers on FreeBSD. The SDK's `sys/_ucontext.h` corrects the console's
machine-context layout, and the platform probe already backpatches 200 fault
sites from a handler. RPCS3 also installs `SIGSEGV`, `SIGBUS`, `SIGILL` and
`SIGPIPE` handlers process-wide. In the title those would sit beside the
title's own crash reporter. *To measure:* a guest page fault handled and
resumed inside the core, with the title's reporter still catching a genuine
crash.

**(h) Threads.** RPCS3's `named_thread` calls `pthread_create`, which the
title binds to its factory (`src/core_threads_ps5.cpp`, 2 MiB minimum stack).
RPCS3 creates a PPU thread for each guest thread, six SPU threads, the RSX
thread and a pool of LLVM compile workers sized from
`sysconf(_SC_NPROCESSORS_ONLN)`. Measured by the thread probe (SDK 9a57ba7):
thirteen CPUs (0–12) are available to the title, and threads run at policy 1,
priority 700, in a range of 256 to 767. A priority set on a running thread
reads back. `pthread_getaffinity_np` with FreeBSD's `cpuset_t` returns ERANGE,
and RPCS3's `set_thread_affinity_mask` uses exactly that form. The platform
layer gets `ps5_pthread_setaffinity_np` and `ps5_pthread_getaffinity_np` (the
exported `scePthreadSetaffinity`/`scePthreadGetaffinity` with a 64-bit mask,
or the size the kernel accepts), bound through `tools/core-imports.py`.
`sysconf(_SC_NPROCESSORS_ONLN)` must answer 13. *To measure:* the affinity
calls round-tripping on the console, and what sysconf answers.

### 4. Risks

| Risk | How it is measured |
| --- | --- |
| LLVM does not build for the PS5 target, or its `Support` layer needs what the console lacks | the LLVM build (step 1) and the core's link and import report (step 3) |
| The core is too large for the loader (1 GiB image cap) | `check-core.py` and the loader's `mapped_bytes` (step 4) |
| Guest reservations do not fit below 1 TiB with the GPU window avoided | the reservation probe (gap b), then `Emu.Init` on the console |
| Emulated TLS costs too much in the hottest paths | the TLS probe (gap e), then the sampler in the game |
| RetroArch's Vulkan interface cannot give RPCS3 the device it needs (1.2 core, extensions) | device creation through `create_device2` (step 4), and RPCS3's own capability log |
| Queue sharing with RetroArch stalls the RSX thread | the sampler on the RSX thread; frame timing |
| Firmware install needs host facilities the title lacks | step 5's install run, file by file |
| Shader compiles cost speed | windows during the game's first minutes with an empty cache |
| The package install fills the console's storage | the install run's free-space check before it starts |
| 16 KiB pages widen the texture cache's invalidations | RPCS3's texture-cache counters in the game |
| The CPU cannot hold full speed (not part of the objective) | the 10 s windows, reported as they are |

### 5. The ladder

Each step ends with its evidence in `docs/ACTIVE.md` and a dated entry in
`docs/PHASE_LOG.md`.

1. **Fork and build scripts.** The fork at the pin; `tools/build-llvm.sh` (the
   pinned static LLVM); `tools/build-ffmpeg.sh` (the pinned FFmpeg);
   `tools/build-rpcs3.sh` on `tools/core-fork.sh`; `tooling/rpcs3/ps5-toolchain.cmake`;
   `rpcs3` in `core_names`. *Evidence:* LLVM and FFmpeg archives with their sizes,
   and `rpcs3_emu` compiling for the PS5 target.
2. **Host build (optional).** The same libretro core for the host, debugged in
   a desktop RetroArch if this host's Vulkan driver allows. *Evidence:* the
   host core reaching the firmware check.
3. **Link and ABI.** The PS5 core links and passes `tools/check-core.py`;
   missing libc and kernel functions go into the platform layer with aliases
   in `tools/core-imports.py`. *Evidence:* `check-core.py` PASS and the import
   list.
4. **Load.** The core loads on the console and `retro_init` returns; the probe
   steps for gaps (a)–(h) run inside the title. *Evidence:* the loader's
   `ready` line, the probe's checks.
5. **Firmware, then a homebrew program.** `PS3UPDAT.PUP` installs from
   `/app0/system/RPCS3/`, then a small PS3 homebrew or test program boots
   before any commercial game. *Evidence:* the firmware version RPCS3 reports,
   and the program's output.
6. **The game.** God of War HD installed from its package and RAP, booted at
   100%, then at 300%: screenshots, the speed of each window, the Close
   Content + reload battery and a ten-minute run. *Evidence:* the screenshot I
   accept, the windows, the battery lines and the ten-minute trace.

## Phase 2: what the port turned out to need

Where the build and the first code differ from the audit above.

**Dependencies.**

- *LLVM* is my fork `../PS5_LLVM` (github.com/mihawk-99/PS5_LLVM), pinned in
  `tools/build-llvm.sh`. The PS4/PS5 ABI ignores `alignas` on an empty base
  class, and LLVM relies on it twice. `SmallVector<T, 0>`'s layout assertions
  failed at build time; the fork aligns `SmallVector` itself under `__SCE__`.
  `TrailingObjects` failed silently: its empty aligner base is what raises a
  class to its trailing objects' alignment, so on the PS5 a header of an `int`
  and some `bool`s stayed 4-aligned and 12 bytes, and two trailing pointers
  were given 28 bytes while `getTrailingObjects` read them from 16 to 32. The
  next allocation's first field overwrote the last pointer's upper half
  (`MachineInstr::ExtraInfo`'s memory operands: God of War HD's SPU compiles
  crashed in `MachineInstr::hasOrderedMemoryRef` on `0x2_xxxx_xxxx`, heap
  pointers with their upper 32 bits replaced by the count 2). The fork gives
  the aligner a zero-length member there, a non-empty base without storage;
  the PS5 target then lays such a class out as the host does (8-aligned, 16
  bytes, 32 allocated, trailing objects at 16). The other empty `alignas`
  classes in LLVM and RPCS3 are standalone objects, whose alignment the ABI
  keeps. The build uses host tablegen
  tools (`LLVM_NATIVE_TOOL_DIR`) and builds only the libraries
  (`LLVM_INCLUDE_TOOLS=OFF`), since no LLVM program links for the console.
- *FFmpeg* 8.1.1 comes from its signed release tarball (`tools/build-ffmpeg.sh`),
  with RPCS3's own component list and NASM for its x86 assembly.
- *GNU libiconv* 1.18, static, comes from its signed release tarball
  (`tools/build-libiconv.sh`). `cellL10n` converts the PS3's character sets
  with iconv, and the console's libc has none.
- *OpenAL Soft* is built rather than compiled out: `cellMic` includes its
  headers outside `WITHOUT_OPENAL` off Android. Every backend is off, so it
  opens no device. Its sources are C++20 modules; CMake's dependency scan runs
  through `tooling/rpcs3/clang-scan-deps`, which gives the scanner the SDK's
  target and headers.
- *zlib* is the frontend's pinned build. libpng's generated configuration does
  not see the submodule's headers.
- *pkg-config* answers nothing in the toolchain file
  (`tooling/rpcs3/ps5-toolchain.cmake`). The host's packages would add the
  host's `/usr/include`.
- The network clients (RPCN, clans, UPnP) still build, with wolfSSL and curl.
  They are not stubbed yet; the title has no network use for them.
- The target defines `__FreeBSD__` as 9, so RPCS3's FreeBSD branches take their
  pre-13 paths (`shm_open(SHM_ANON)` for `memfd_create`). The PS5 branch below
  replaces them where memory is concerned.

**Vulkan** (`rpcs3/Emu/RSX/VK/vkutils/swapchain_libretro.*` in the fork).
RetroArch creates the instance with the core version RPCS3 asks for (1.2), and
the device through `create_device2`. The core runs RPCS3's own
`render_device::create` there, so the device has RPCS3's features and
extensions plus RetroArch's; when the renderer starts, `render_device::create`
runs again and adopts that device instead of creating one. Features RPCS3
enables only for some settings are the same whatever the game's configuration,
so the device a boot needs is the one RetroArch made. Every submission and
device-wide wait takes RetroArch's queue lock inside RPCS3's own submit lock.
The swapchain is the core's own images, sampled by RetroArch:

- acquiring an image signals RPCS3's semaphore with an empty submission;
- presenting waits on RPCS3's semaphore and signals a fence;
- `retro_run` hands RetroArch the newest finished image with `set_image`, after
  its fence;
- an image RetroArch showed is reused only after RetroArch's frame index that
  last read it comes around again, which is when RetroArch has waited for that
  frame.

The images are not RPCS3's allocations, so they outlive the renderer while
RetroArch may still show one; they go when a newer frame replaces them or the
context is destroyed. The game boots in the first `retro_run` after RetroArch's
context is up, because RPCS3 builds its renderer while booting.

**Input** (`rpcs3/libretro/libretro_pad_handler.*`). RetroPads 1 to 4 are the
PS3's controllers, through a pad handler on RPCS3's generic machinery (SDL's
value conventions). The core compiles `rpcs3/Input/pad_thread.cpp` and what it
needs, without the HID handlers (no hidapi on the console), and writes the
input configuration that names the RetroPads before the pad thread loads it.

**Memory** (the `__PROSPERO__` branch of `rpcs3/util/vm_native.cpp`). RPCS3's
reservations, commits and shared memory go through the platform layer:

- reservations are `ps5_vrange_reserve` and `ps5_vrange_reserve_at`;
- commits and decommits are `ps5_vrange_commit` and `ps5_vrange_decommit`,
  direct memory in 64 KiB units (SDK 7f3d3ff);
- `utils::shm` is a `ps5_shm` direct-memory object with its views.

Guest memory starts at 64 GiB (`0x10_0000_0000`). The 32 GiB hook area is only
reserved: nothing reads it, and a 32 GiB copy-on-write view of zeros has no
direct-memory form. The page size is the kernel's 16 KiB. The core's
reservations are released, with the memory committed in them, when the core is
unloaded, so the next load finds its addresses free.

**Faults.** In the libretro core, a fault RPCS3 does not handle goes back to the
handler it replaced (the title's crash reporter), and the title's handlers
return when the core is unloaded.

**Platform gaps closed in the SDK (8160289).** Each is aliased in
`tools/core-imports.py`.

- No export at all: `accept4`, `getpagesizes`, `in6addr_any`.
- Exported, but resolved to nothing in a title: `gai_strerror`, `statfs`,
  `fstatfs`, `umask`, `fork`, `setsid`, `wait4`.
- Formerly taken from the SDK's static payload libc, whose system calls a title
  may not make: `syscall` (only `SYS_write`), `getpwnam_r`, `posix_madvise`,
  `strsignal`.
- Answered differently on the console: `pthread_getaffinity_np` and
  `pthread_setaffinity_np` (the 64-bit mask), `sysconf` (13 CPUs, not 16),
  `pthread_exit` (a thread's `thread_local` destructors first).
- Refused: `realpath` answers EPERM, even for `/app0`, so libc++'s
  `std::filesystem::canonical` and `weakly_canonical` came back empty and the
  package installer could not normalise its path. `ps5_realpath` makes the path
  absolute, resolves `.` and `..` by name and checks each component with
  `stat`; it keeps a symbolic link's name. The title links it too
  (`--wrap=realpath`).
- Memory: committed ranges (`ps5_vrange_commit`), exact reservations, zeroing
  of reused direct memory, and placement up to 1 TiB.

The title's core loader answers a core's `dlopen(NULL)` with the process handle
(`src/core_loader_ps5.cpp`), which LLVM's JIT needs.

**What the console run needed in the fork** is recorded in docs/PHASE_LOG.md
(2026-09-28): RSX's start order, VMA's entry points under volk, the kqueue
trigger RSX audio's timer uses, releasing LLVM's memory-manager ranges, the
log listeners' lifetime, and quitting through `Emulator::Quit`.

**Content loading and the loading screen** (`rpcs3/libretro/libretro_loader.*`
in the fork). `retro_load_game` returns at once: the firmware and a PSN package
install on a worker thread while `retro_run` shows a loading screen, and the
game boots when they are done. The screen is drawn on the CPU (the stb
libraries RPCS3 already has) and handed to RetroArch as a Vulkan image, one
per RetroArch frame index, with the copy submitted through
`set_command_buffers`:

- a violet night behind the game's own art (its PIC1, blurred and tinted),
  its ICON0 on a glass card, the title in Inter, and a violet-to-orchid bar
  with a sweeping highlight;
- "Installing firmware" with its packages, "Installing" with the bytes, the
  speed, the time left and the percentage, then "Starting" with RPCS3's
  compile progress, until RPCS3's first frame replaces it;
- what failed, in rose, when something does.

Inter 4.1 (SIL Open Font License, its licence beside it) is fetched by
`tools/build-rpcs3.sh`, pinned by digest, and staged in `system/RPCS3/fonts`;
the firmware's Rodin is the fallback. A finished package install is recorded
in `system/RPCS3/libretro/installed/<title ID>`, since an interrupted one leaves
an `EBOOT.BIN` behind, and the record goes when an install starts. Closing
content during an install stops it, inside a large file too (the fork's
`unpkg.cpp`); a partial firmware is installed again next time.

**RPCS3's folder, as staged.** Besides the loading screen's fonts, the title
stages RPCS3's own overlay images (`bin/Icons/ui` into `system/RPCS3/Icons/ui`),
which its native dialogs draw. The configuration the core writes at load also
turns RPCS3's GDB server off: no debugger connects to a core, and it would only
open a socket on the console. Open question: the server's address did not
match RPCS3's IPv4 `std::regex` on the console (it fell through to the Unix
socket branch), which may mean `std::regex` misbehaves there; RPCS3 also parses
patches and its game database with it, so it wants a probe.

**What loads.** A PSN package (`.pkg`, installed once, then booted at
once), a game's `EBOOT.BIN`, an ISO, or a folder (RetroArch's "Use this
directory"): the folder's `USRDIR/EBOOT.BIN` (an installed game, such as
`system/RPCS3/dev_hdd0/game/NPUA80490`) or `PS3_GAME/USRDIR/EBOOT.BIN` (a
disc game) boots. RPCS3 itself treats a bare folder as "build the PPU cache of
everything in it", which compiled and stopped. Loading a folder also first
crashed the title: canonicalising a relative path asks libc++ for the working
directory, and the title's own `getcwd` resolves to nothing on the console
(libc's calls `__getcwd`, which only libkernel_sys has). The title links it to
the platform's `ps5_getcwd` (`--wrap=getcwd`), as the cores already were, and
`tooling/native/app-symbols.map` keeps every `__wrap_` symbol local, since lld
exported `__wrap_getcwd` to interpose on libc's stubs and a title publishes
no exports.

**Core options.** The resolution scale is a libretro v2 option, 100% to 300%
(2160p), 300% by default, written to `config.yml` when content loads, as
RPCS3's settings dialog writes it, so a game's custom configuration still
overrides it.

**A title's write budget.** A title writes at full speed for a burst of a few
GiB and then at about 2 MiB/s, buffered or direct, while another process
writes the same folder faster (the SDK's docs/PROBE.md, "Sustained writes").
God of War HD's 6.3 GB package took 31.5 minutes to install. Installing a
package's large files without copying them, read and decrypted from the
package the way RPCS3 reads an ISO through a virtual device, would remove the
wait and the second copy.
