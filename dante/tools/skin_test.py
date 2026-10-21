#!/usr/bin/env python3
"""Check the native skinning (src/dante_skin_impl.h) against the recompiled original, on the Mac.

    python3 dante/tools/skin_test.py [runs]

Builds, with the host clang, one program holding the recompiled function (copied from dante/out/codegen, the code
generated from your executable) and the native one, and runs both on the same random inputs: vertices, bone matrices
and vertex counts in a 4 GB guest address space. Everything they write is compared: every memory page either touched
(outputs, bounding box, stack, the zeroed cache lines) and the non-volatile registers (r1, r13-r31, LR). Prints the
time of each per vertex. Exit code 0 only if every run matched.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "dante/out/codegen/generated/default"
OUT = ROOT / "dante/out/skin_test"
SDK = ROOT / "sdk"

# (recompiled function, native function in dante::skin)
PAIRS = [("SkinVerts4", "SkinVerts<4>"), ("SkinVerts3", "SkinVerts<3>"), ("SkinVerts1", "SkinVerts<1>")]

FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{\n", re.M)
CALL = re.compile(r"\b([A-Za-z_]\w*)\(ctx, base\);")


def body(name: str) -> str:
    for f in sorted(GEN.glob("*.cpp")):
        t = f.read_text()
        m = re.search(rf"^DEFINE_REX_FUNC\({name}\) \{{\n", t, re.M)
        if m:
            nxt = FUNC.search(t, m.end())
            b = t[m.end():nxt.start() if nxt else len(t)]
            return b[:b.rstrip().rfind("}")]
    sys.exit(f"{name} not in {GEN} (run dante/tools/codegen.sh)")


MAIN = r'''
static const size_t kSize = (size_t(1) << 32) + (1 << 20);
static uint8_t* Map() {
  void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); _exit(2); }
  return static_cast<uint8_t*>(p);
}
// Guest layout of a test: constants come from the real image (copied at their addresses), the rest is random.
constexpr uint32_t kVerts = 0x40000000, kBones = 0x41000000, kOut = 0x42000000, kBox = 0x43000000, kStack = 0x44000000;
static const bool raw = getenv("SKIN_RAW") != nullptr;
// Two differing words that are both NaN (a NaN's payload can depend on which operand an instruction takes first).
static bool AmbosNaN(const uint8_t* a, const uint8_t* b) {
  uint32_t x, y; std::memcpy(&x, a, 4); std::memcpy(&y, b, 4); x = __builtin_bswap32(x); y = __builtin_bswap32(y);
  return (x & 0x7F800000) == 0x7F800000 && (x & 0x7FFFFF) && (y & 0x7F800000) == 0x7F800000 && (y & 0x7FFFFF);
}
static void Fill(uint8_t* m, const std::vector<uint8_t>& image, uint64_t seed, uint32_t count) {
  std::memcpy(m + 0x82000000u, image.data(), image.size());
  std::mt19937_64 r(seed);
  auto fill = [&](uint32_t a, uint32_t n, bool floats) {
    for (uint32_t i = 0; i < n; i += 4) {
      uint32_t v = uint32_t(r());
      // Floats as the game has them (finite); SKIN_RAW=1 also puts raw bit patterns (NaNs) in one word of 8.
      if (floats && (!raw || (r() & 7))) { float f = float(int32_t(v % 20001) - 10000) / 997.0f; std::memcpy(&v, &f, 4); }
      v = __builtin_bswap32(v);
      std::memcpy(m + a + i, &v, 4);
    }
  };
  fill(kVerts, 32 * count + 64, true);
  fill(kBones, 256 * 64, true);
  fill(kOut, 20 * count + 512, false);
  fill(kBox, 64, false);
  fill(kStack - 4096, 8192, false);
}
static void Init(PPCContext& c, uint64_t seed, uint32_t count) {
  std::mt19937_64 r(seed ^ 0x5A);
  uint8_t* p = reinterpret_cast<uint8_t*>(&c);
  for (size_t i = 0; i < sizeof(c); ++i) p[i] = uint8_t(r());
  for (int i = 0; i < 32; ++i) (&c.r3)[i].u64 = r() & 0xFFFFFFFF;
  c.r1.u64 = kStack - (seed % 16) * 16;
  c.r3.u64 = kVerts; c.r4.u64 = count; c.r5.u64 = kBones;
  c.r7.u64 = kOut + (seed % 5) * 4; c.r8.u64 = kBox; c.r9.u64 = kBox + 16;
  c.lr = 0x82123450;
  c.fpscr = PPCContext{}.fpscr;
  c.reserved_address = ~uint64_t(0);
}
int main(int argc, char** argv) {
  const int runs = argc > 1 ? atoi(argv[1]) : 200;
  std::vector<uint8_t> image;
  { FILE* f = fopen(IMAGE, "rb"); if (!f) { perror(IMAGE); return 2; } fseek(f, 0, SEEK_END); image.resize(ftell(f)); fseek(f, 0, SEEK_SET); fread(image.data(), 1, image.size(), f); fclose(f); }
  uint8_t* A = Map(); uint8_t* B = Map();
  const size_t pg = getpagesize();
  std::vector<char> va(kSize / pg), vb(kSize / pg);
  int bad = 0;
  for (auto& par : kPares) {
    double ta = 0, tb = 0; uint64_t verts = 0; int ok = 0, nan_runs = 0;
    for (int s = 1; s <= runs; ++s) {
      const uint32_t count = 1 + uint32_t(s * 2654435761u % 96);
      Fill(A, image, s, count); Fill(B, image, s, count);
      alignas(64) PPCContext ca, cb; Init(ca, s, count); std::memcpy(&cb, &ca, sizeof(ca));
      auto t0 = std::chrono::steady_clock::now(); par.a(ca, A);
      auto t1 = std::chrono::steady_clock::now(); par.b(cb, B);
      auto t2 = std::chrono::steady_clock::now();
      ta += std::chrono::duration<double, std::micro>(t1 - t0).count();
      tb += std::chrono::duration<double, std::micro>(t2 - t1).count();
      verts += count;
      bool same = ca.r1.u64 == cb.r1.u64 && ca.lr == cb.lr;
      for (int i = 13; i <= 31 && same; ++i) if ((&ca.r3)[i].u64 != (&cb.r3)[i].u64) { printf("%s run %d: r%d differs\n", par.n, s, i); same = false; }
      if (!same) printf("%s run %d: r1/lr differ\n", par.n, s);
      mincore(A, kSize, va.data()); mincore(B, kSize, vb.data());
      for (size_t p = 0; p < va.size() && same; ++p) {
        if (!(va[p] & 1) && !(vb[p] & 1)) continue;
        if (std::memcmp(A + p * pg, B + p * pg, pg)) {
          size_t k = 0; while (A[p * pg + k] == B[p * pg + k]) ++k;
          if (raw) {  // with NaNs in the inputs: only NaN payloads may differ
            bool solo_nan = true;
            for (size_t w = 0; w < pg; w += 4)
              if (std::memcmp(A + p * pg + w, B + p * pg + w, 4) && !AmbosNaN(A + p * pg + w, B + p * pg + w)) solo_nan = false;
            if (solo_nan) { ++nan_runs; continue; }
          }
          printf("%s run %d (count %u): memory differs at guest 0x%zx\n", par.n, s, count, p * pg + k); same = false;
          if (getenv("SKIN_DEBUG")) {
            const size_t at = p * pg + k - (p * pg + k - cb.r7.u32 + 0) % 20;
            printf("  original:"); for (int j = 0; j < 20; ++j) printf(" %02x", A[at + j]);
            printf("\n  native:  "); for (int j = 0; j < 20; ++j) printf(" %02x", B[at + j]); printf("\n");
          }
        }
      }
      ok += same; bad += !same;
      munmap(A, kSize); munmap(B, kSize); A = Map(); B = Map();
    }
    printf("%-12s %d/%d runs identical (%d of them only in NaN payloads); recompiled %.1f ns/vertex, native %.1f "
           "ns/vertex (x%.1f)\n", par.n, ok, runs, nan_runs, ta * 1000 / verts, tb * 1000 / verts, ta / tb);
  }
  return bad ? 1 : 0;
}
'''


def main() -> None:
    runs = sys.argv[1] if len(sys.argv) > 1 else "200"
    OUT.mkdir(parents=True, exist_ok=True)
    parts = ['#include "dantes_inferno_pch.h"', '#include "dante_skin_impl.h"', "#include <sys/mman.h>",
             "#include <chrono>", "#include <cstdio>", "#include <cstring>", "#include <random>", "#include <vector>",
             "#include <unistd.h>", f'#define IMAGE "{ROOT / "dante/out/codegen/image.bin"}"']
    callees = set()
    bodies = {}
    for orig, _ in PAIRS:
        bodies[orig] = body(orig)
        callees |= set(CALL.findall(bodies[orig]))
    # The callees the originals make (the register save and restore helpers), copied too.
    for c in sorted(callees):
        parts.append(f"extern \"C\" REX_FUNC({c}) {{\n{body(c)}}}")
    for orig, _ in PAIRS:
        parts.append(f"REX_FUNC(original_{orig}) {{\n{bodies[orig]}}}")
    parts.append("using Fn = void (*)(PPCContext&, uint8_t*);")
    parts.append("struct Par { const char* n; Fn a, b; };")
    parts.append("Par kPares[] = {" + ", ".join(
        f'{{"{o}", original_{o}, [](PPCContext& c, uint8_t* b) {{ dante::skin::{n}(c, b); }}}}' for o, n in PAIRS) + "};")
    parts.append(MAIN)
    (OUT / "skin_test.cpp").write_text("\n".join(parts))
    inc = []
    for d in ["dante/out/sw/generated_nx", "dante/src", "sdk/include", "sdk/thirdparty/simde", "sdk/thirdparty/fmt/include",
              "sdk/thirdparty/spdlog/include", "sdk/thirdparty/xxHash", "sdk/thirdparty/tomlplusplus/include"]:
        inc += ["-I", str(ROOT / d)]
    subprocess.check_call(["clang++", "-std=c++23", "-O2", "-fno-strict-aliasing", "-ffp-contract=off", "-w",
                           "-DSPDLOG_FMT_EXTERNAL", *inc, str(OUT / "skin_test.cpp"), "-o", str(OUT / "skin_test")])
    sys.exit(subprocess.call([str(OUT / "skin_test"), runs]))


if __name__ == "__main__":
    main()
