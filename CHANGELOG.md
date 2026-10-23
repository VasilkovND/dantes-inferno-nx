# Changelog

## 1.1.0 (TU2) — 2026-10-23

- **The game's CPU work is faster** (`dante/tools/patch_stack_access.py`, `patch_inline_calls.py`,
  `patch_local_regs.py`, from gears2-nx 1.0.0; [dante/CPU_OPTIMIZATION.md](dante/CPU_OPTIMIZATION.md)). The recompiled
  code kept every PowerPC register in memory and moved it in and out around every guest load and store; now each
  function keeps them in host registers and writes back only what other code can read, as worked out for the whole
  program. At stock clocks, the heavy scene after loading a save went from ~38 fps to 44-46 fps, where it is now bound
  by the GPU (the frame takes ~1 ms more than the GPU's 21 ms). The NRO is 36.5 MB instead of 44.2.
- Porting the pass found an unsound case in it (calls inside `if { ...; return; }` blocks, which Hell's Gate writes
  134 times): a check build (`PLR_CHECK=1`) crashed in the menu, the fix is in and the check build plays clean.
- **Debug modes removed**: the debug shortcuts (ZL + ZR + Down / Left / Right / R: captures, frame dumps,
  draw-skipping modes, stack sampling) are off whatever `dante.toml` says; telemetry (`telemetria.flag`: automatic
  captures, frame dumps, frame and draw traces, CPU stack sampling, GPU profile), `sin_resplandor.flag`,
  `dante_shader_dump` and `dante_vp6_comprobar` are gone. The FPS counter stays (**ZL + ZR + Up**). Shader containers
  the library lacks are still written to `shaders_dump/`.
- Only the NRO changes: the shader library and the DKSH cache are the same as 1.0.0's.

## 1.0.3 (TU2) — 2026-10-23

- **Resolved textures of five sizes at one address** (`sdk-patches/0011`): the renderer kept four other sizes per
  address and dropped by key order, so the 1280x720 and 1280x360 ones the game resolves every frame were destroyed and
  created again (empty) twice a frame from the first circles on: wasted clears and copies, and effects that sample the
  older one read an empty image. Now six per address, the least recently used dropped first.
- **FPS Locker** (`sdk-patches/0012`): the target its overlay sets (handheld and docked, and the one it saved for the
  title) is honoured as NX-FPS does for Vulkan games. Without a target the game is not limited.
- Only the NRO changes: the shader library and the DKSH cache are the same as 1.0.0's.
- Heavy scenes that drop to 30 fps overclocked are bound by the GPU (15-27 ms a frame at 614 MHz), not the CPU: the
  game's Presentation thread shows ~95 % at any clock because it waits busily. GPU 768 MHz helps; CPU above ~1785 MHz
  does not.

## 1.0.2 (TU2) — 2026-10-22

- **Building from a Games on Demand install.** `dante/tools/build_mac.sh` (and `codegen.sh`) take the package folder
  itself: `default.xex` is read out of its `DataNNNN` pieces (`dante/tools/god_xex.py`), and no `default.xexp` is
  needed for an executable that already has Title Update 2. The code generated from it is identical to the Europe
  disc's (only codegen's stamps differ). In 1.0.1 the build still asked for `default.xex` + `default.xexp`.
- The program is unchanged from 1.0.1 apart from its version.

## 1.0.1 (TU2) — 2026-10-22

- **Games on Demand installs run as they are.** Copy the package from the Xbox 360 hard drive
  (`Content/0000000000000000/454108CF/00007000/`: the header file and its `.data` folder with every `DataNNNN`) into
  `sdmc:/switch/dantes-inferno-nx/god/`; nothing to extract. The package is read as the disc (`d:`), its `default.xex`
  loaded from `god_xex/`, which the program fills itself. Tested on the console with the 49028C6A package.
- **Edition 49028C6A.** Its Games on Demand executable already carries Title Update 2 (0.0.2.1), and its disc
  executable with its own TU2 loads to the same image byte for byte as the Europe one with its TU2: the same build runs
  all of them. `tools/stage_sd.py` accepts the 49028C6A disc with `TU_12K226F_0000004000000.00000000000G1`.
- The SDK's package reader takes one read at a time (`sdk-patches/0010`): the game reads on several threads.

## 1.0.0 (TU2) — 2026-10-21

First release: Dante's Inferno is playable on the Switch, natively (homebrew NRO). Played on a console from the start
through the first levels, overclocked and at stock clocks; a full playthrough has not been verified yet.

Nothing from the game is distributed: the recompiled code, the shaders and the NRO are all made on your computer from
your own disc and Title Update 2 ([INSTALL.md](INSTALL.md)).

### Code

- The game's executable (Europe disc, Media ID 27C36C6C, with Title Update 2) recompiled to C++ by ReXGlue v0.10.0 at
  build time (`dante/tools/codegen.sh`), with the codegen manifest and fixes of [Hell's Gate](https://github.com/florinp93/hells-gate-recomp),
  the PC port, fetched at a pinned commit: 36,715 functions.
- The save system's guest setjmp/longjmp run as host setjmp/longjmp (`dante/src/dantes_inferno_hooks.h`).
- An indirect call to an address with no recompiled function logs and returns instead of aborting
  (`sdk-patches/0007`).

### Rendering

- The native deko3d renderer of ds2-nx (PM4 ring reader), with the game's 1,359 shaders extracted from the executable
  and translated ahead of time; every variant is precompiled (`shadercache_dksh.bin`), none is compiled on the console in
  normal play.
- The console's own shader cache file holds only what the console compiled (a few hundred KB, not the whole 30 MB again
  after each new variant), and each variant compiled during play is logged for the PC tool (`dante/dksh_variants.txt`).

### Videos

- The FMVs (EA VP6, 1280x720) are decoded by FFmpeg's VP6 decoder instead of the game's, which recompiled made about
  18 of their 30 frames a second: 5-9 ms a frame overclocked, 15-21 at stock clocks.

### CPU

- The game's CPU skinning (4, 3 and 1 bones per vertex) runs natively, with results identical bit for bit to the
  recompiled loops (checked on the Mac by `dante/tools/skin_test.py`), about three times faster.
- RenderWare Audio: MixWithGain / CopyWithGain on NEON, and the final compressor's own bypass, as in ds2-nx.
- The recompiled code's "interrupts off" regions are per thread (no process-wide mutex), as in ds2-nx.

### Performance

| | Overclocked (CPU 1785 MHz, GPU 768 MHz) | Stock clocks (CPU 1020 MHz, GPU 307 MHz) |
| --- | --- | --- |
| Most scenes | 60 fps (the game's cap) | 40-55 fps (GPU-bound: 16-22 ms a frame) |
| Heaviest scenes | 45-60 fps | 22-30 fps |

### Known issues

- At stock clocks the GPU bounds the frame rate; the renderer has no lower internal resolution.
- Only the Europe disc (En/De/It) with its Title Update 2 is supported; German or Italian text (`user_language = 3` or
  `6` in `dante.toml`) has not been tried.
- DLC (Trials of Saint Lucia) is not supported.
