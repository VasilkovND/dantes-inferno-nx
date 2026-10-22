#!/bin/bash
# Generate the recompiled C++ of Dante's Inferno (TU2) from your own executable, and its memory image the shaders are
# extracted from. Nothing here is committed: the output is dante/out/codegen/.
#
#   dante/tools/codegen.sh <folder with default.xex and default.xexp>     (tools/stage_sd.py's game/ folder works)
#   dante/tools/codegen.sh <Games on Demand install folder>               (its default.xex already has TU2)
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
  sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi
GAME=$(cd "$1" && pwd)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/dante/out"
CG="$OUT/codegen"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"

HELLS_GATE_COMMIT="3ce3993b0f03c77353f8f10fdcd982160c9a2d18"   # 2026-10-16, v0.8.4-beta
REXGLUE_TAG="v0.10.0"
# The executables this build is made and tested from; all load to the same image byte for byte (dante/PORTING.md).
TESTED="3840af42c628d0d8d6d98965057893b8bfc7c197479f485fe6f57db8b014b0aa+f49c87b8d16ef0eed693c134458ccb2bd05481ef24f38b7c6d60d532163d2290
abfef19fa03a247ab17d5c011e34ca70a0338f13c15b7fd8932c7ff7d9e2c2e9+0842733820a27e0c902e6a652b3cd71d2c4a05f36db5121c1d342416a6b7ab09
e905710ce74cf9390e0e23463db16d52968f3c5bf4670f60b966a8d1a6ac4469+"
#   Europe disc 27C36C6C + its TU2; disc 49028C6A + its TU2; the 49028C6A Games on Demand executable (TU2 in it)
XEX_YA_TU2="e905710ce74cf9390e0e23463db16d52968f3c5bf4670f60b966a8d1a6ac4469"

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
mkdir -p "$OUT"
XEX="$GAME/default.xex"
XEXP="$GAME/default.xexp"
if [ ! -f "$XEX" ] && find "$GAME" -name Data0000 -print -quit | grep -q .; then
  echo "== default.xex from the Games on Demand package"
  python3 "$ROOT/dante/tools/god_xex.py" "$GAME" "$OUT/god_default.xex"
  XEX="$OUT/god_default.xex"
fi
[ -f "$XEX" ] || { echo "$GAME/default.xex not found"; exit 1; }
if [ "$(sha "$XEX")" = "$XEX_YA_TU2" ]; then
  XEXP=""  # Title Update 2 is in it already: no update beside it (patching an updated executable fails)
elif [ ! -f "$XEXP" ]; then
  echo "$XEXP not found (Title Update 2 for your disc)"; exit 1
fi
if ! printf '%s\n' "$TESTED" | grep -qx "$(sha "$XEX")+${XEXP:+$(sha "$XEXP")}"; then
  echo "warning: default.xex / default.xexp are not ones this build was tested with (see dante/PORTING.md); trying anyway"
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
cmp -s "$XEX" "$CG/game/default.xex" || cp "$XEX" "$CG/game/default.xex"
if [ -n "$XEXP" ]; then
  cmp -s "$XEXP" "$CG/game/default.xexp" || cp "$XEXP" "$CG/game/default.xexp"
else
  rm -f "$CG/game/default.xexp"
fi
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
INPUTS=$(cat "$CG/game/default.xex" $( [ -f "$CG/game/default.xexp" ] && echo "$CG/game/default.xexp") "$CG/dantes_inferno_manifest.toml" \
  "$CG/patches/generated/apply_generated_patches.py" "$REXGLUE" | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat "$STAMP" 2>/dev/null)" != "$INPUTS" ] || [ ! -f "$CG/generated/default/sources.cmake" ]; then
  # Generated and fixed in a staging folder, then only the files whose text changed are copied over generated/:
  # the others keep their dates, so after a small change (a name in names.toml) Ninja rebuilds a few units, not 135.
  echo "== codegen"
  ST="$CG/staging"
  rm -rf "$ST"
  mkdir -p "$ST/patches/generated"
  cp "$CG/dantes_inferno_manifest.toml" "$ST/"
  cp "$CG/patches/generated/apply_generated_patches.py" "$ST/patches/generated/"
  ln -s ../game "$ST/game"
  (cd "$ST" && REX_DUMP_IMAGE="$CG/image.bin" "$REXGLUE" --force codegen dantes_inferno_manifest.toml > "$CG/codegen.log" 2>&1) || {
    tail -20 "$CG/codegen.log"; exit 1; }
  echo "== generated-code fixes (Hell's Gate)"
  (cd "$ST" && python3 patches/generated/apply_generated_patches.py > "$CG/apply_generated_patches.log")
  tail -1 "$CG/apply_generated_patches.log"
  grep -q 'Remaining REX_FATAL unresolved traps: 0 ' "$CG/apply_generated_patches.log" || {
    echo "unresolved branches left in the generated code (see $CG/apply_generated_patches.log)"; exit 1; }
  python3 - "$ST/generated/default" "$CG/generated/default" <<'PY'
import filecmp, os, shutil, sys
new, old = sys.argv[1:]
os.makedirs(old, exist_ok=True)
names = set(os.listdir(new))
changed = 0
for n in sorted(names):
    a, b = os.path.join(new, n), os.path.join(old, n)
    if not os.path.isfile(b) or not filecmp.cmp(a, b, shallow=False):
        shutil.copy2(a, b)
        changed += 1
for n in set(os.listdir(old)) - names:
    os.remove(os.path.join(old, n))
print(f"generated/default: {changed} of {len(names)} files changed")
PY
  rm -rf "$ST"
  echo "$INPUTS" > "$STAMP"
fi
echo "codegen: $(ls "$CG"/generated/default/*.cpp | wc -l | tr -d ' ') files in $CG/generated/default, image $CG/image.bin"
