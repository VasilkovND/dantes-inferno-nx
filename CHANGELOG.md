# Changelog

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
