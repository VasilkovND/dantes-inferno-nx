# dantes-inferno-nx

Dante's Inferno (Xbox 360, Title Update 2) running natively on the Nintendo Switch. It's not an emulator: the game's
PowerPC code is statically recompiled to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) (with the codegen
manifest and fixes of [Hell's Gate](https://github.com/florinp93/hells-gate-recomp), the PC port), built for the Switch
as a homebrew NRO, and frames are drawn by a native deko3d renderer from the game's translated shaders. It follows
[ds2-nx](https://github.com/VasilkovND/ds2-nx), the Dead Space 2 port: same studio, same engine family and libraries.

> [!NOTE]
> You need your own copy of the game (the disc and Title Update 2). This repository contains no game data or game
> code: the code, the shaders and the NRO are all made on your computer from your copy.

Status: **1.1.0** — playable. Overclocked: 60 fps (the game's cap) in most scenes; stock clocks: 44-60 fps, the GPU
bounding it in heavy scenes. Installation: [INSTALL.md](INSTALL.md); what changed and the known issues: [CHANGELOG.md](CHANGELOG.md);
details in [dante/PORTING.md](dante/PORTING.md).

## Layout

| Folder | Contents |
| --- | --- |
| `dante/` | The game project: CMake, Switch glue (`src/`: app, GPU ring reader and renderer glue, shader dump and library, audio and CPU hooks, the setjmp/longjmp helpers the generated code calls), build scripts (`tools/`), hook names for the codegen (`codegen/`), our SDK patches (`sdk-patches/`) and host-tool patches (`host-patches/`) |
| `shaders/` | Shader toolchain: extraction from the executable, Xenos microcode -> HLSL -> SPIR-V -> shader library and DKSH cache ([shaders/README.md](shaders/README.md)) |
| `sdk/` | **Submodule** -> [`rexglue-sdk-nx`](https://github.com/nx-mod/rexglue-sdk-nx) (`switch` branch): the ReXGlue SDK + Horizon (Switch) layer + deko3d renderer. Our changes to it live as patches in `dante/sdk-patches/`, applied by the build script |
| `tools/switch/cmake/` | `switch-devkitA64.cmake`, the CMake toolchain file for devkitA64 |

## Build

devkitA64, libnx and deko3d from [devkitPro](https://devkitpro.org) (`sudo dkp-pacman -S switch-dev deko3d`), CMake,
Ninja, Python 3, clang and the [Vulkan SDK](https://vulkan.lunarg.com) (`dxc`, `spirv-val`). On a Mac:

```sh
python3 dante/tools/stage_sd.py --disc "Dante's Inferno.iso" --tu TU_12K226F_000000C000000.00000000000G3 -o sd/dantes-inferno-nx
dante/tools/build_mac.sh sd/dantes-inferno-nx/game        # or the Games on Demand install folder
```

`build_mac.sh` runs, in order:

1. `dante/tools/codegen.sh`: fetches Hell's Gate at a pinned commit and ReXGlue v0.10.0, builds ReXGlue's `rexglue`
   tool for the Mac with Hell's Gate's SDK patch, generates the C++ from your `default.xex` + `default.xexp` (36,715
   functions) with Hell's Gate's manifest plus `dante/codegen/names.toml`, and applies Hell's Gate's fixes to it.
2. `dante/tools/shaders.sh`: extracts the 1,359 shader containers from the executable image and builds
   `dante_shaders.dslib` and `shadercache_dksh.bin` (every variant precompiled for the console).
3. The NRO: `dante/out/sw/dante.nro`.

## SD card

Either the extracted disc in `game/`:

```
sdmc:/switch/dantes-inferno-nx/
    dantes-inferno-nx.nro     dante/out/sw/dante.nro
    dante_shaders.dslib       dante/out/shaders/
    shadercache_dksh.bin      dante/out/shaders/
    game/                     the extracted disc + default.xexp (Title Update 2), by stage_sd.py
```

or a Games on Demand install, as the Xbox 360 hard drive has it, in `god/`:

```
sdmc:/switch/dantes-inferno-nx/
    dantes-inferno-nx.nro
    dante_shaders.dslib
    shadercache_dksh.bin
    god/                      the contents of Content/0000000000000000/454108CF/00007000/
        <header file>
        <header file>.data/Data0000 ... DataNNNN
```

Supported: the Europe disc (Media ID 27C36C6C) and the 49028C6A disc with their Title Update 2, and the 49028C6A
Games on Demand package (Title Update 2 already in it). The program creates `god_xex/`, `cache/`, `logs/` and its saves.

## Credits

- **[florinp93](https://github.com/florinp93/hells-gate-recomp)**: Hell's Gate, the PC port of Dante's Inferno. Its
  codegen manifest, SDK codegen fixes and generated-code fixes are what makes the recompiled code run; they are
  fetched at build time.
- **[nx-mod](https://github.com/nx-mod/rexglue-sdk-nx)**: rexglue-sdk-nx, the Switch fork of the SDK, and its Halo 3
  port, where the PM4 ring reader and the universal deko3d renderer started.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)**: NFSMW-NX: the Switch layer, deko3d renderer and shader tools.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)**: the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)**: XenosRecomp.
- **[centollOS](https://github.com/centollOS/SwitchWakerHD)**: the uam shader compiler patches.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**: devkitA64, libnx
  and deko3d. **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: the custom firmware.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

Unofficial fan port with no affiliation to Electronic Arts or Visceral Games. "Dante's Inferno" is a trademark of
Electronic Arts. You provide your own legally obtained copy of the game. No prebuilt files are distributed: the NRO
contains code recompiled from the game and the shader library the game's own shaders, so both are made on your own
computer from your copy ([INSTALL.md](INSTALL.md)). Running homebrew requires custom firmware, which violates
Nintendo's terms and can get a console banned.

**Official source.** The only official source of this project is this repository,
[github.com/VasilkovND/dantes-inferno-nx](https://github.com/VasilkovND/dantes-inferno-nx): its source code and its releases. I do not
publish ready-made NRO or NSP files, and never anything with game data. Builds, NSP/XCI packages or archives offered on
other sites (ROM sites, file hosts, forums, chats) are not made, endorsed or checked by me, may be modified, and are
not covered by this project; distributing the game's files that way infringes its rights holders' copyright and is
solely the responsibility of whoever does it.

Source code is under the GPL-3.0 license (see [LICENSE](LICENSE)). The SDK changes are under the SDK's BSD-3-Clause
license, and the shader translator changes under MIT.
