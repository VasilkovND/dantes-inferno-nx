# Porting notes: Dante's Inferno

Dante's Inferno (Xbox 360, Title Update 2) on the Nintendo Switch, as a homebrew NRO. The second port after
[ds2-nx](https://github.com/VasilkovND/ds2-nx) (Dead Space 2): same studio (Visceral), same engine family and EA
libraries, same route. Its PORTING.md is the reference for everything this file does not repeat.

## Status (1.0.0)

Playable, played on the console through the first levels. The game targets 60 fps.

| | Overclocked (CPU 1785 MHz, GPU 768 MHz) | Stock clocks (CPU 1020 MHz, GPU 307 MHz) |
| --- | --- | --- |
| Most scenes | 60 fps | 40-55 fps, GPU-bound (16-22 ms a frame) |
| Heaviest scenes | 45-60 fps | 22-30 fps (33-45 ms a frame on the GPU) |

## Architecture

```
default.xex (Europe disc) + TU2 default.xexp         (your copy, tools/stage_sd.py)
        │
tools/codegen.sh                                     ReXGlue v0.10.0 `rexglue` built for the Mac with Hell's Gate's SDK
        │                                            patch; Hell's Gate's manifest + codegen/names.toml; then Hell's
        │                                            Gate's generated-code fixes. 36,715 functions, 135 units
rexglue-sdk-nx (sdk/, + sdk-patches/)                kernel, memory, threads, audio, input, deko3d presenter and the
        │                                            rex::x360 deko3d renderer
dante/src/
    dante_gpu.cpp          PM4 ring reader (ds2-nx's, game-neutral)
    dante_shaders.cpp      identifies each shader the game loads in the library
    dante_shader_dump.cpp  hooks D3DDevice_Create{Vertex,Pixel}Shader; writes containers missing from the library
    dante_audio.cpp        NEON MixWithGain / CopyWithGain
    dante_cpu.cpp          mtmsrd regions per thread
    dantes_inferno_hooks.h the host setjmp/longjmp the generated code calls (saves and loads)
shaders/                                             containers from the executable image → HLSL → SPIR-V → DKSH
```

## 1. Code

- **Hell's Gate** (florinp93/hells-gate-recomp, the PC port) does not check in generated code: its build runs ReXGlue's
  codegen on the player's executable. So does this one (`tools/codegen.sh`), at Hell's Gate's tested commit
  (`HELLS_GATE_COMMIT`), with three of its pieces, fetched and never copied (no license file):
  1. its patch to ReXGlue v0.10.0: codegen fixes (data-section function-pointer scan, `lvebx/lvehx/lvewx`,
     `vmaddcfp128`, vector pack operand order), top-level `[[midasm_hook]]` in the manifest, and the PCH including
     `dantes_inferno_hooks.h`; plus PC renderer and logging changes this build does not use;
  2. its manifest: 339 function hints for TU2 (most `unresolved_target_*`). Its one mid-asm hook (ultrawide aspect)
     is dropped;
  3. `apply_generated_patches.py`: the guest setjmp/longjmp of the save system become host setjmp/longjmp at every
     call site, and 181 branches the codegen left as traps become calls/gotos. 0 traps remain on TU2.
- **TU2 only.** The disc executable alone (base) codegens too, but the manifest is TU2's: 7 of its addresses are past
  the base code and 8 traps remain. Title Update 1 fails the codegen's validation with this manifest.
- **No symbol names**: every function is `sub_XXXXXXXX`. The ones the Switch code hooks get names from
  `codegen/names.toml`, found by matching function bodies against Dead Space 2's PDB-named recompiled code
  (instruction sequences with branch targets and address halves masked): 4,252 of DS2's names match a TU2 function
  exactly (rw::audio 300+, EARS, Havok, VP6, XAudio2, D3DX, XDK D3D).
- The Switch PCH is ds2-nx's `make_nx_pch.py` unchanged (GCC `DEFINE_REX_FUNC`, per-thread `mtmsrd`).
- `FiberSetjmp` is a macro, not a function: GCC never inlines a function that calls setjmp, and a setjmp in a frame
  that has returned cannot be jumped back to.
- `sdk-patches/0007`: an indirect call to an address with no function logs once and returns instead of aborting
  (Hell's Gate does the same: some vtable slots are only known at run time); the XMA context store no longer writes
  back the guest-owned output read offset.
- `OnPreLaunchModule` checks that the image is TU2 (its setjmp, 0x82879110) and clears the save system's fiber-switch
  callback (0x82CE68E4), as Hell's Gate does.

## 2. Discs

One disc, extracted to `game/` (the runtime maps both `game:` and `d:` there), with TU2's `default.xexp` beside
`default.xex`. bigfile0.viv 3.17 GB, bigfile1.viv 2.70 GB: FAT32 works. `stage_sd.py` checks both hashes.

| File | SHA-256 |
| --- | --- |
| Europe `default.xex` (Media ID 27C36C6C, v0.0.0.3) | `3840af42c628d0d8d6d98965057893b8bfc7c197479f485fe6f57db8b014b0aa` |
| TU2 `default.xexp` (`TU_12K226F_000000C000000.00000000000G3`, image v0.0.2.3) | `f49c87b8d16ef0eed693c134458ccb2bd05481ef24f38b7c6d60d532163d2290` |

Games on Demand: `god/` next to the NRO may hold the package as the Xbox 360 hard drive has it (header file and
`<name>.data/DataNNNN`). `src/dante_god.cpp` mounts it with the SDK's `StfsContainerDevice` (Xenia's SVOD reader, plain
file reads) as `d:` and `\\Device\\Cdrom0` (where the game opens its bigfiles), loads its `default.xex` from `god_xex/`
and maps `game:` to the package after the load. `sdk-patches/0010` serialises that reader's seek-and-read on the shared
data files (the game reads on several threads).

The user's package is Media ID 49028C6A. Its `default.xex` is version 0.0.2.1: Title Update 2 already applied (the
disc's executable, 0.0.0.1, is beside it as `default.dll`). Loaded, it is byte for byte the image this build was
made from (Europe disc + TU2, 0.0.2.3), as is that disc executable with its own TU2
(`TU_12K226F_0000004000000.00000000000G1`): one build runs all three. So no update is put beside an already updated
executable (patching it would fail), and `stage_sd.py` accepts both discs.

| File | SHA-256 |
| --- | --- |
| 49028C6A disc `default.xex` (`default.dll` in the GOD package, v0.0.0.1) | `abfef19fa03a247ab17d5c011e34ca70a0338f13c15b7fd8932c7ff7d9e2c2e9` |
| 49028C6A TU2 `default.xexp` | `0842733820a27e0c902e6a652b3cd71d2c4a05f36db5121c1d342416a6b7ab09` |
| 49028C6A GOD `default.xex` (v0.0.2.1) | `e905710ce74cf9390e0e23463db16d52968f3c5bf4670f60b966a8d1a6ac4469` |

## 3. Shaders

- Unlike Dead Space 2, the shaders are not in the bigfiles: all 50,625 RefPack payloads of the `ols3`/`SHOC` assets
  decompress and none holds a 2008-format container. They are in the **executable**: the loaded TU2 image (dumped by
  the host codegen, `host-patches/0001`) holds 1,403 containers, 1,359 unique (1,214 PS, 145 VS).
  `shaders/di_extract_shaders.cpp` writes them under the same XXH3 key the console's create hooks compute, so the
  library is complete before the first boot; whatever the game creates that is not in it is still dumped on the
  console (`shaders_dump/`).
- All 1,359 translate (XenosRecomp with ds2-nx's fixes, `--saltos-lineales`), validate, and precompile: 12,492 DKSH
  variants, 30 MB.
- Open: whether the containers the game passes to D3D are byte for byte the ones in the image (if it patches them
  first, the keys differ and the console dumps them).

## 4. Console sessions

| Run | Build | What happened |
| --- | --- | --- |
| 1 | 936b5ea | Crash after 11 s in the XDK's draw path (0x827EA4A0). The hook names `D3DDevice_Create{Vertex,Pixel}Shader` were also the names of the SDK's export stubs (`xam_misc.cpp`), so every shader creation called a stub. Fixed with the address in the names; `codegen.sh` now refuses a name the SDK defines. |
| 2 | 4b772cf | Plays. Shaders: every container the game created was in the library (the offline extraction gives exactly the bytes the game passes to D3D), `shaders_dump/` stayed empty. Stutters and drops: 56 DKSH variants compiled in the background in an hour (exact vertex-shader interpolator sets, early-depth pixel shaders), each seconds of a core; after each one the console rewrote the whole 30 MB cache to the SD card. |
| 3 | 34bdd9e | Quit at start: the shader worker at priority 0x3F. A homebrew process may not go below 0x3B, which is also the only priority Horizon time-slices and where every guest thread runs, so the worker stays at 0x3B. |

What came of run 2 (`sdk-patches/0008`, `dksh_variants.txt`):
- The console's cache file holds only what it compiled, not the PC tool's cache again.
- Each background compile is logged with its cache key; `tools/dksh_variants.py` turns those lines into
  `dante/dksh_variants.txt`, which `ds_dksh --variantes` compiles into `shadercache_dksh.bin`. Precompiling every exact
  vertex-shader and early-depth pixel-shader variant instead was measured: 29,673 variants, 117 MB of cache, which the
  renderer keeps in memory, against 30 MB now.
- `rw::audio::core::Compressor1::Process` (0x824463B8) takes its own bypass, as in ds2-nx: Dead Space 2's code at other
  constant and branch addresses (91 of 92 instructions).

## 5. Videos

The FMVs are EA VP6, 1280x720 at 29.97 fps (34 movies in bigfile0). The game decodes them on its `Job Manager` thread;
recompiled, its decoder made ~18 frames a second with that thread at 95 % of a core (run 4's stack profile, taken during
a video with the VP6 functions named: `VP6_DecodeBlock` 0x82697A00 and its motion filters 0x8269C1F8 / 0x8269C2A8 are
nearly all of it; float IDCT on VMX).

- Registers in locals (`tools/localize_regs.py`, a copy of a function with the guest registers as local variables) was
  tried first and checked on the Mac against the original with random inputs (identical registers and memory in 40
  runs): ×1.02 on warm data with clang. The context is `__restrict`; the cost is the VMX code itself. Not used.
- FFmpeg's VP6 (`sdk-patches/0009` adds it to the SDK's FFmpeg) decodes these movies at ~1.1 ms a frame on one Mac
  core. `src/dante_video.cpp` hooks `VP6_DecodeFrameMbs` (0x826974D0, the macroblock pass, named in `codegen/names.toml`):
  at a frame's first row it decodes the whole frame into the game's frame buffer and marks every row done; the header
  parse and the buffer rotation stay the game's. The first frame of each decoder instance is decoded by both and
  compared (as is and upside down) to learn the buffer's orientation and check the layout; a video whose pictures do
  not match stays with the game's decoder. `dante_vp6_ffmpeg=false` turns it off.

### Orientation

The game's buffer holds each frame upside down against FFmpeg's picture, three ways: FFmpeg's VP6 (not VP6F) decodes
with `flip = -1`, the bitstream's first macroblock row being the picture's bottom row; the game's row pass (0x8269C360)
addresses a row as `frame + (16 * row - 48) * stride` with a positive stride, so that first row is at the start of the
buffer; and session 5, which wrote FFmpeg's picture as it is (its calibration decided on a black frame), showed the
videos upside down. So frames are written mirrored (`dante_vp6_comprobar=true` compares with the game's decoder again).
FFmpeg takes 4.5-9 ms a frame overclocked, 12-20 at stock, of the 33 a frame lasts.

## 6. Stock clocks (CPU 1020 MHz, GPU 307 MHz)

Session 5: 38-50 fps in ordinary scenes, 22-25 in the heaviest. Mostly the GPU: 16-22 ms a frame (scene pass 9-14, up
to 25; copies 3.5-6.3; passes without depth 1.5-9). No supersampling: the scene is 1280x720 at 1x; only clears and a
quarter-size (640x360) pass are 4x multisampled, drawn as their 1x alias. On the CPU the game's Presentation thread is
at 84 % (up to 97 %).

### Skinning

About an eighth of the Presentation thread, and a share of the EARS job workers, is the game's CPU skinning: three VMX
loops, 4, 3 and 1 bones per vertex (0x825B6E80, 0x825B6BC8, 0x825B6778, named SkinVerts4/3/1 in `codegen/names.toml`).
Each blends its bone matrices (64 bytes, three rows), moves the position (w = 1), normal and tangent, renormalises and
packs the last two to signed bytes (x128, truncated, clamped), writes 20 bytes per vertex and grows a bounding box; it
also zeroes the cache line 128 bytes ahead (dcbzl) and saves its registers on the stack.

`src/dante_skin_impl.h` does the same per vertex with the same vector operations (the simde calls the codegen emits, in
host layout) in registers, without the software pipelining, the context round trips and the byte-at-a-time stores
(stvlx / stvrx). `tools/skin_test.py` builds both for the Mac and compares them on random inputs in a 4 GB guest space:
every touched page and the non-volatile registers. 3,000 runs each: identical; with raw bit patterns (NaNs) in the
inputs, only NaN payloads differ. 2.2-3.3x faster on an M1 core. `dante_skin_nativo=false` runs the recompiled loops.

## 7. Not ported yet

- Dead Space 2's per-stage audio timers, its `Dac::Process` hook and the `EA::Thread::SetThreadPriority` shortcut: those
  functions differ in this game and no clear match was found (the compressor's was: section 4).
- A lower internal render resolution, the one lever left against the GPU at stock clocks: the renderer keys targets by
  EDRAM base and pitch and has no scaled mode.
- Disc swap (one disc here), DLC, and the game icon (none from the game).
