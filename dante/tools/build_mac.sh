#!/bin/bash
# Build dantes-inferno-nx.nro with devkitPro's toolchain on a Mac (Apple Silicon or Intel); on Linux the same steps with
# your distribution's packages instead of Homebrew (not tested).
#
# Once:
#   1. devkitPro: install the pkg from https://github.com/devkitPro/pacman/releases (or
#      https://devkitpro.org/wiki/Getting_Started), then:  sudo dkp-pacman -S switch-dev deko3d
#   2. Homebrew tools:  brew install cmake ninja python git
#   3. The Vulkan SDK (https://vulkan.lunarg.com/, for dxc and spirv-val: the shaders)
#
# Then, from the repository root, with the folder tools/stage_sd.py made (or any folder with your default.xex and
# Title Update 2's default.xexp):
#   dante/tools/build_mac.sh <game folder>
# It generates the code (tools/codegen.sh), the shaders (tools/shaders.sh, once) and the NRO:
#   dante/out/sw/dante.nro                    -> sdmc:/switch/dantes-inferno-nx/dantes-inferno-nx.nro
#   dante/out/shaders/dante_shaders.dslib     -> beside it
#   dante/out/shaders/shadercache_dksh.bin    -> beside it
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ $# -eq 1 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
GAME="$1"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/tools/bin:$PATH"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"

[ -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++" ] || { echo "devkitA64 not found in $DEVKITPRO (step 1)"; exit 1; }
[ -f "$DEVKITPRO/libnx/lib/libdeko3d.a" ] || { echo "deko3d not installed: sudo dkp-pacman -S deko3d"; exit 1; }

cd "$ROOT"
if [ ! -f sdk/CMakeLists.txt ]; then
  echo "== sdk submodule"
  git submodule update --init sdk
fi
if [ ! -f sdk/thirdparty/uam/uam_api.h ]; then
  echo "== Switch third-party sources"
  (cd sdk && python3 tools/switch_setup.py)
fi

# Fixes to the SDK that the game needs and that are not upstream yet (dante/sdk-patches/).
for p in "$ROOT"/dante/sdk-patches/*.patch; do
  [ -f "$p" ] || continue
  if git -C sdk apply --reverse --check "$p" 2>/dev/null; then
    echo "== sdk patch already applied: $(basename "$p")"
  else
    echo "== sdk patch: $(basename "$p")"
    git -C sdk apply "$p"
  fi
done

"$ROOT/dante/tools/codegen.sh" "$GAME"
if [ ! -f dante/out/shaders/shadercache_dksh.bin ] || [ "${SHADERS:-0}" = 1 ]; then
  "$ROOT/dante/tools/shaders.sh"
fi

# Configure only the first time (or with RECONFIGURE=1): afterwards Ninja re-runs CMake by itself when a CMake file
# changes.
if [ ! -f dante/out/sw/build.ninja ] || [ "${RECONFIGURE:-0}" = 1 ]; then
  echo "== configure"
  cmake -S dante -B dante/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
    -DREXSDK_DIR="$ROOT/sdk" -DDANTE_CODEGEN_DIR="$ROOT/dante/out/codegen" -DREXGLUE_SWITCH_DEKO3D=ON
fi

echo "== build ($JOBS jobs)"
cmake --build dante/out/sw -j "$JOBS"
ls -la dante/out/sw/dante.nro dante/out/shaders/dante_shaders.dslib dante/out/shaders/shadercache_dksh.bin
