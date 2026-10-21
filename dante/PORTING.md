# Porting notes: Dante's Inferno

Dante's Inferno (Xbox 360, Title Update 2) on the Nintendo Switch, as a homebrew NRO. The second port after
[ds2-nx](https://github.com/VasilkovND/ds2-nx) (Dead Space 2): same studio (Visceral), same engine family and EA
libraries, same route. Its PORTING.md is the reference for everything this file does not repeat.

## Status (0.1.0, in development)

Boots, renders and plays on the console (tested with CPU 1785 MHz / GPU 768 MHz). The game targets 60 fps and holds it
in most of what was played; it drops to 45-48 fps where the CPU is short (3 cores) and to 30 where a frame takes the GPU
17-20 ms. The FMVs stutter (section 5). Not tested at stock clocks yet.

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

The FMVs are VP6, decoded in software by the game on its `Job Manager` thread, which sits at ~95 % of a core for the
whole video (Presentation ~85 %, MoviePlayer2 ~30 %, the ring thread ~30 % uploading the frames). From the texture
uploads, about 16-19 decoded frames per second. Same open problem as Dead Space 2 (same EA VP6 decoder). The decoder's
functions are named in `codegen/names.toml` (31, matched to Dead Space 2's by instruction sequence; `VP6_DecodeBlock` is
0x82697A00) for a stack profile taken during a video, which decides between native NEON versions of the hottest
functions and decoding the frames with FFmpeg's VP6.

## 6. Not ported yet

- Dead Space 2's other audio hooks (compressor bypass, per-stage timers, `Dac::Process`) and the
  `EA::Thread::SetThreadPriority` shortcut: those functions differ slightly in this game, so no exact match.
- Disc swap (one disc here) and the game icon (none from the game).
