#!/bin/bash
# Generate the recompiled C++ of Dante's Inferno (TU2) from your own default.xex + default.xexp, and the executable's
# memory image the shaders are extracted from. Nothing here is committed: the output is dante/out/codegen/.
#
#   dante/tools/codegen.sh <folder with default.xex and default.xexp>     (tools/stage_sd.py's game/ folder works)
#
# Steps (each skipped when already done):
#   1. Hell's Gate (https://github.com/florinp93/hells-gate-recomp), the PC port, at the tested commit: its codegen
#      manifest (function hints for TU2), its patch to ReXGlue v0.10.0 (codegen fixes) and its script that fixes the
#      generated code (guest setjmp/longjmp, unresolved branches). It has no license file, so it is fetched, not copied.
#   2. ReXGlue SDK v0.10.0 for the host, with that patch and ours (host-patches/: dump the loaded image), built with
#      the Mac's clang: only its `rexglue` tool is used.
#   3. The manifest: Hell's Gate's, without its ultrawide mid-asm hook (PC only), plus codegen/names.toml.
#   4. rexglue codegen, then Hell's Gate's apply_generated_patches.py. The build stops if any unresolved branch is left.
set -euo pipefail

if [ $# -ne 1 ]; then
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi
GAME=$(cd "$1" && pwd)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/dante/out"
CG="$OUT/codegen"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"

HELLS_GATE_COMMIT="3ce3993b0f03c77353f8f10fdcd982160c9a2d18"   # 2026-10-16, v0.8.4-beta
REXGLUE_TAG="v0.10.0"
XEX_SHA="3840af42c628d0d8d6d98965057893b8bfc7c197479f485fe6f57db8b014b0aa"   # disc default.xex (Europe, 27C36C6C)
XEXP_SHA="f49c87b8d16ef0eed693c134458ccb2bd05481ef24f38b7c6d60d532163d2290"  # TU2 default.xexp for that disc

for f in default.xex default.xexp; do
  [ -f "$GAME/$f" ] || { echo "$GAME/$f not found"; exit 1; }
done
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
if [ "$(sha "$GAME/default.xex")" != "$XEX_SHA" ] || [ "$(sha "$GAME/default.xexp")" != "$XEXP_SHA" ]; then
  echo "warning: default.xex / default.xexp are not the tested ones (Europe disc 27C36C6C + its TU2); trying anyway"
fi

# 1. Hell's Gate
HG="$OUT/hells-gate-recomp"
if [ "$(git -C "$HG" rev-parse HEAD 2>/dev/null)" != "$HELLS_GATE_COMMIT" ]; then
  echo "== Hell's Gate $HELLS_GATE_COMMIT"
  rm -rf "$HG"
  mkdir -p "$HG"
  git -C "$HG" init -q
  git -C "$HG" remote add origin https://github.com/florinp93/hells-gate-recomp.git
  git -C "$HG" fetch -q --depth 1 origin "$HELLS_GATE_COMMIT"
  git -C "$HG" checkout -q FETCH_HEAD
fi

# 2. Host ReXGlue
HOST="$OUT/rexglue-host"
REXGLUE="$HOST/out/$(uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/mac/')-$(uname -m | sed 's/x86_64/amd64/')/rexglue"
if [ ! -x "$REXGLUE" ]; then
  if [ ! -d "$HOST/.git" ]; then
    echo "== ReXGlue $REXGLUE_TAG (host)"
    git clone -q --branch "$REXGLUE_TAG" --depth 1 https://github.com/rexglue/rexglue-sdk.git "$HOST"
    git -C "$HOST" submodule update --init --recursive --depth 1 -j 8
  fi
  for p in "$HG/patches/sdk/rexglue-sdk-v0.10.0.patch" "$ROOT"/dante/host-patches/*.patch; do
    if git -C "$HOST" apply --reverse --check "$p" 2>/dev/null; then continue; fi
    echo "== host patch: $(basename "$p")"
    git -C "$HOST" apply "$p"
  done
  echo "== build rexglue (host)"
  cmake -S "$HOST" -B "$HOST/out/host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_USE_VULKAN=ON > "$OUT/rexglue-host.log"
  cmake --build "$HOST/out/host" --target rexglue -j "$JOBS" >> "$OUT/rexglue-host.log"
  if [ "$(uname -s)" = Darwin ]; then
    # Its libraries sit beside it: find them there, so the folder can move.
    for p in $(otool -l "$REXGLUE" | awk '/LC_RPATH/{getline; getline; print $2}'); do
      install_name_tool -rpath "$p" @executable_path "$REXGLUE" 2>/dev/null || true
    done
    codesign -f -s - "$REXGLUE" 2>/dev/null || true
  fi
fi

# 3. Project folder: the executable (copied: codegen refuses a link out of its game root) and the manifest.
mkdir -p "$CG/game" "$CG/patches/generated"
for f in default.xex default.xexp; do
  cmp -s "$GAME/$f" "$CG/game/$f" || cp "$GAME/$f" "$CG/game/$f"
done
python3 - "$HG/dantes_inferno_manifest.toml" "$ROOT/dante/codegen/names.toml" "$CG/dantes_inferno_manifest.toml" <<'PY'
import re, sys
text, extra, out = open(sys.argv[1]).read(), open(sys.argv[2]).read(), sys.argv[3]
text = re.sub(r'\[\[midasm_hook\]\]\n(?:(?!\[)[^\n]+\n)*\n?', '', text)
for addr in re.findall(r'^\[entrypoint\.functions\.(0x[0-9A-Fa-f]+)\]', extra, re.M):
    if f'[entrypoint.functions.{addr}]' in text:
        sys.exit(f'names.toml: {addr} is already in the manifest')
new = text.rstrip() + '\n\n' + extra
try:
    if open(out).read() == new:
        sys.exit(0)
except OSError:
    pass
open(out, 'w').write(new)
PY
cp "$HG/patches/generated/apply_generated_patches.py" "$CG/patches/generated/"
# A name the SDK already defines as __imp__<name> (its export stubs) would replace the game's function with that stub.
for n in $(sed -n 's/^name = "\(.*\)"$/\1/p' "$ROOT/dante/codegen/names.toml"); do
  if grep -rqE "__imp__$n\b" "$ROOT/sdk/src" "$ROOT/sdk/include"; then
    echo "codegen/names.toml: the SDK already defines __imp__$n; pick another name"; exit 1
  fi
done

# 4. Codegen (re-run only when an input changed) and the generated-code fixes.
STAMP="$CG/inputs.sha"
INPUTS=$(cat "$CG/game/default.xex" "$CG/game/default.xexp" "$CG/dantes_inferno_manifest.toml" \
  "$CG/patches/generated/apply_generated_patches.py" "$REXGLUE" | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat "$STAMP" 2>/dev/null)" != "$INPUTS" ] || [ ! -f "$CG/generated/default/sources.cmake" ]; then
  echo "== codegen"
  rm -rf "$CG/generated"
  (cd "$CG" && REX_DUMP_IMAGE="$CG/image.bin" "$REXGLUE" --force codegen dantes_inferno_manifest.toml > codegen.log 2>&1) || {
    tail -20 "$CG/codegen.log"; exit 1; }
  echo "== generated-code fixes (Hell's Gate)"
  (cd "$CG" && python3 patches/generated/apply_generated_patches.py > apply_generated_patches.log)
  tail -1 "$CG/apply_generated_patches.log"
  grep -q 'Remaining REX_FATAL unresolved traps: 0 ' "$CG/apply_generated_patches.log" || {
    echo "unresolved branches left in the generated code (see $CG/apply_generated_patches.log)"; exit 1; }
  echo "$INPUTS" > "$STAMP"
fi
echo "codegen: $(ls "$CG"/generated/default/*.cpp | wc -l | tr -d ' ') files in $CG/generated/default, image $CG/image.bin"
