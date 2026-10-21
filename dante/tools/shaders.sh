#!/bin/bash
# The deko3d shader library and DKSH cache, from your own copy: shaders are pulled out of the executable image that
# tools/codegen.sh dumped (out/codegen/image.bin), translated ahead of time (shaders/build_library.sh) and compiled for
# the console's GPU, so the game never compiles a shader while it runs. Nothing here is committed.
#
#   dante/tools/shaders.sh        -> dante/out/shaders/dante_shaders.dslib and shadercache_dksh.bin (copy beside the NRO)
#
# Needs the Vulkan SDK's dxc and spirv-val (VULKAN_BIN, see shaders/README.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/dante/out"
IMAGE="$OUT/codegen/image.bin"
[ -f "$IMAGE" ] || { echo "$IMAGE not found: run dante/tools/codegen.sh first"; exit 1; }
mkdir -p "$OUT/bin"
clang++ -std=c++20 -O2 -I"$ROOT/sdk/thirdparty/xxHash" "$ROOT/shaders/di_extract_shaders.cpp" -o "$OUT/bin/di_extract_shaders"
rm -rf "$OUT/shaders_extracted"
"$OUT/bin/di_extract_shaders" "$OUT/shaders_extracted" "$IMAGE"
# The variants the console compiled in the background in earlier sessions (tools/dksh_variants.py) go in the cache too.
DKSH=1 VARIANTES="$ROOT/dante/dksh_variants.txt" "$ROOT/shaders/build_library.sh" "$OUT/shaders_extracted" "$OUT/shaders"
ls -la "$OUT/shaders/dante_shaders.dslib" "$OUT/shaders/shadercache_dksh.bin"
