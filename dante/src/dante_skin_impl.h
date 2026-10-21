// dantes-inferno-nx - native skinning (see dante_skin.cpp). In a header so the Mac test (tools/skin_test.sh) builds the
// same code against the recompiled originals.
//
// Three versions of one loop, by bones per vertex: SkinVerts4 (0x825B6E80), SkinVerts3 (0x825B6BC8), SkinVerts1
// (0x825B6778, rigid: the bone's matrix as it is, no weights). Position, normal and tangent.
//   r3  input vertices, 32 bytes each: position x, y, z (big-endian floats) and the normal as 4 signed bytes (+12), the
//       tangent as 4 signed bytes (+16), the bone indices as 4 bytes (+20), the weights as 4 big-endian u16 (+24;
//       the first 3 with 3 bones, none with 1)
//   r4  vertex count (at least 1)
//   r5  bone matrices, 64 bytes each: three rows of 4 floats
//   r7  output vertices, 20 bytes each: x, y, z, the normal (4 bytes) and the tangent (4 bytes)
//   r8  the bounding box's minimum, r9 its maximum (16-byte vectors, written at the end)
// Per vertex: the weighted sum of its bone matrices (((m0 w0 + m1 w1) + m2 w2) + m3 w3), the position (w = 1)
// through it, the normal and the tangent through it, renormalised, scaled by 128, truncated and clamped to [-128, 127]
// and packed; the box grows by the position.
//
// The recompiled loop is software-pipelined and keeps every register in the PPCContext; it also stores its outputs a
// byte at a time (stvlx / stvrx). This does one vertex at a time with the very same vector operations (the simde
// calls the codegen emits, in host register layout) in locals, so the results are bit for bit the same, and repeats the
// original's other memory effects: its saved registers (r27-r31, r28-r31 with 1 bone) and LR on the stack, a constant at
// r1-64 (not with 1 bone), and the cache-line
// zeroing (dcbzl) of the line 128 bytes past each output vertex. Non-volatile registers end as they started.

#pragma once

#include "dantes_inferno_pch.h"

#include <cstring>

namespace dante::skin {

inline simde__m128i Rev(simde__m128i v) {
  return simde_mm_shuffle_epi8(v, simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(VectorMaskL)));
}
inline simde__m128i Lvx(uint8_t* base, uint32_t a) {
  return Rev(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(REX_RAW_ADDR(a & ~0xFu))));
}
inline void Stvx(uint8_t* base, uint32_t a, simde__m128i v) {
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(REX_RAW_ADDR(a & ~0xFu)), Rev(v));
}
inline uint32_t Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// Signed bytes of the word at PPC element 3 / 0 of v, as 4 floats / 128 (vspltw, vupkhsb twice, vcsxwfp 7).
inline simde__m128 Bytes(simde__m128i v, int splat) {
  simde__m128i s = simde_mm_shuffle_epi32(v, 0);
  if (splat == 0xFF) s = simde_mm_shuffle_epi32(v, 0xFF);
  s = simde_mm_cvtepi8_epi16(simde_mm_unpackhi_epi64(s, s));
  s = simde_mm_cvtepi16_epi32(simde_mm_unpackhi_epi64(s, s));
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(s), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3C000000))));
}

// vmrghw a, b in host layout.
inline simde__m128 Mrghw(simde__m128 a, simde__m128 b) {
  return simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(simde_mm_castps_si128(b), simde_mm_castps_si128(a)));
}

// Renormalise v (3 components), put w in element 3, scale by 128, truncate, clamp.
inline simde__m128i Packed(simde__m128 v, simde__m128 w, simde__m128i lo, simde__m128i hi) {
  const simde__m128 len = simde_mm_dp_ps(v, v, 0xEF);
  const simde__m128 rsq = simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(len));
  v = simde_mm_mul_ps(rsq, v);
  v = simde_mm_blend_ps(v, simde_mm_permute_ps(w, 147), 1);
  simde__m128i q = rex::ppc::simde_mm_vctsxs(simde_mm_mul_ps(v, simde_mm_set1_ps(128)));
  q = simde_mm_min_epi32(q, hi);
  return simde_mm_max_epi32(q, lo);
}

template <int kHuesos>
inline void SkinVerts(PPCContext& ctx, uint8_t* base) {
  static_assert(kHuesos == 1 || kHuesos == 3 || kHuesos == 4);
  constexpr uint32_t K = 0x820C44B0u;  // the functions' constants (lis -32244, addi 17584)
  // Prologue: mflr r12; bl __savegprlr_27 (r27-r31 at r1-48, r12 at r1-8), __savegprlr_28 with 1 bone.
  const uint32_t sp = ctx.r1.u32;
  if (kHuesos > 1) REX_STORE_U64(sp - 48, ctx.r27.u64);
  REX_STORE_U64(sp - 40, ctx.r28.u64);
  REX_STORE_U64(sp - 32, ctx.r29.u64);
  REX_STORE_U64(sp - 24, ctx.r30.u64);
  REX_STORE_U64(sp - 16, ctx.r31.u64);
  REX_STORE_U32(sp - 8, uint32_t(ctx.lr));

  const simde__m128i c112 = Lvx(base, K - 112);
  const simde__m128 uno = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c112, 0xFF));  // 1.0
  simde__m128 caja_min = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c112, 0xAA));
  simde__m128 caja_max = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(c112, 0x55));
  const simde__m128i m_pesos = Lvx(base, K - 64), m_normal = Lvx(base, K - 96), m_tangente = Lvx(base, K - 32);
  const simde__m128i lo = Lvx(base, K), hi = Lvx(base, K - 16);
  if (kHuesos > 1) Stvx(base, sp - 64, m_tangente);

  const uint32_t n = ctx.r4.u32, entrada = ctx.r3.u32, huesos = ctx.r5.u32;
  uint32_t salida = ctx.r7.u32;
  for (uint32_t i = 0; i < n; ++i, salida += 20) {
    const uint32_t a = entrada + 32 * i;
    const simde__m128i raw0 = Lvx(base, a), raw1 = Lvx(base, a + 16);
    const uint32_t idx = REX_LOAD_U32(a + 20);
    const simde__m128 p = simde_mm_blend_ps(simde_mm_castsi128_ps(raw0), simde_mm_permute_ps(uno, 147), 1);
    const simde__m128 nn = Bytes(raw0, 0), tt = Bytes(raw1, 0xFF);
    const simde__m128 w = simde_mm_mul_ps(
        rex::ppc::simde_mm_cvtepu32_ps_(rex::ppc::simde_mm_perm_epi8_(raw1, simde_mm_castps_si128(uno), m_pesos)),
        simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x37800000))));
    const simde__m128i wi = simde_mm_castps_si128(w);
    const simde__m128 w0 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(wi, 0xFF));
    const simde__m128 w1 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(wi, 0xAA));
    const simde__m128 w2 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(wi, 0x55));
    const simde__m128 w3 = simde_mm_castsi128_ps(simde_mm_shuffle_epi32(wi, 0x00));
    const uint32_t o0 = huesos + (Rotl(idx, 14) & 0x3FC0), o1 = huesos + (Rotl(idx, 22) & 0x3FC0),
                   o2 = huesos + (Rotl(idx, 30) & 0x3FC0), o3 = huesos + (Rotl(idx, 6) & 0x3FC0);
    simde__m128 b[3];
    for (uint32_t k = 0; k < 3; ++k) {
      if (kHuesos == 1) {
        b[k] = simde_mm_castsi128_ps(Lvx(base, o0 + 16 * k));
        continue;
      }
      simde__m128 t = simde_mm_mul_ps(simde_mm_castsi128_ps(Lvx(base, o0 + 16 * k)), w0);
      t = simde_mm_add_ps(simde_mm_mul_ps(simde_mm_castsi128_ps(Lvx(base, o1 + 16 * k)), w1), t);
      t = simde_mm_add_ps(simde_mm_mul_ps(simde_mm_castsi128_ps(Lvx(base, o2 + 16 * k)), w2), t);
      b[k] = kHuesos == 4 ? simde_mm_add_ps(simde_mm_mul_ps(simde_mm_castsi128_ps(Lvx(base, o3 + 16 * k)), w3), t) : t;
    }
    const simde__m128 px = simde_mm_dp_ps(p, b[0], 0xFF), py = simde_mm_dp_ps(p, b[1], 0xFF),
                      pz = simde_mm_dp_ps(p, b[2], 0xFF);
    const simde__m128 nx = simde_mm_dp_ps(nn, b[0], 0xEF), ny = simde_mm_dp_ps(nn, b[1], 0xEF),
                      nz = simde_mm_dp_ps(nn, b[2], 0xEF);
    const simde__m128 tx = simde_mm_dp_ps(tt, b[0], 0xEF), ty = simde_mm_dp_ps(tt, b[1], 0xEF),
                      tz = simde_mm_dp_ps(tt, b[2], 0xEF);
    const simde__m128 pos = Mrghw(Mrghw(px, pz), py);
    // As the original merges them: (x, z) first, then y.
    const simde__m128 normal = Mrghw(Mrghw(nx, nz), ny);
    const simde__m128 tangente = Mrghw(Mrghw(tx, tz), ty);
    caja_min = simde_mm_min_ps(caja_min, pos);
    caja_max = simde_mm_max_ps(caja_max, pos);
    const simde__m128i qn = Packed(normal, simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(nn), 0)),
                                   lo, hi);
    const simde__m128i qt = Packed(tangente, simde_mm_castsi128_ps(simde_mm_shuffle_epi32(simde_mm_castps_si128(tt), 0)),
                                   lo, hi);
    // dcbzl 128 bytes ahead, then the vertex: 16 bytes (stvlx / stvrx) and the tangent word (stvewx).
    std::memset(REX_RAW_ADDR((salida + 128) & ~127u), 0, 128);
    const simde__m128i v16 = Rev(rex::ppc::simde_mm_perm_epi8_(simde_mm_castps_si128(pos), qn, m_normal));
    std::memcpy(REX_RAW_ADDR(salida), &v16, 16);
    alignas(16) uint32_t t4[4];
    simde_mm_store_si128(reinterpret_cast<simde__m128i*>(t4), rex::ppc::simde_mm_perm_epi8_(qt, qt, m_tangente));
    const uint32_t ea = (salida + 16) & ~3u;
    REX_STORE_U32(ea, t4[3 - ((ea & 0xF) >> 2)]);
  }
  Stvx(base, ctx.r8.u32, simde_mm_castps_si128(caja_min));
  Stvx(base, ctx.r9.u32, simde_mm_castps_si128(caja_max));
  // Epilogue: b __restgprlr_27 / _28 (those registers and LR as they were; r12 = LR).
  ctx.r12.u64 = REX_LOAD_U32(sp - 8);
  ctx.lr = ctx.r12.u64;
}

}  // namespace dante::skin
