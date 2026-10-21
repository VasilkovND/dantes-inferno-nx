// dantes-inferno-nx - lighter audio for the Switch's CPU.
//
// Dante's Inferno mixes with the same RenderWare Audio (rwaudiocore) as Dead Space 2, where the mixer thread was the
// heaviest after the game's own; here too it takes 25-40 % of a core in fights (RwAudioCore Dac), plus the XMA decoder.
// dante/codegen/names.toml names the functions hooked here: MixWithGain and CopyWithGain are byte for byte Dead
// Space 2's, Compressor1::Process the same code at other addresses. Dead Space 2's per-stage timers are not ported.
//
// The final compressor (rw::audio::core::Compressor1::Process and the CompressorLimiter1 loop it calls) was a third of
// Dead Space 2's mixer: per-sample gain in dB through VMX log/exp estimates, translated to scalar code. It has its own
// bypass: with its threshold at 0 dB or its ratio at 1 it leaves the mixer's buffer as it is (it works in place), clears
// its state and returns 1. dante_audio_sin_compresor takes that path always. What is lost is the dynamics compression
// of the final mix: loud moments are not squeezed (the output is still clamped).

#include "dantes_inferno_pch.h"

#include <cstring>

#include <rex/cvar.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(dante_audio_sin_compresor, true, "Dante",
                    "Saltarse el compresor final del audio del juego (su propio camino de bypass): parte del hilo de "
                    "mezcla. false = el sonido original, mas CPU");

REX_EXTERN(__imp__rw__audio__core__Compressor1__Process);
REX_EXTERN(rw__audio__core__Compressor1__Process) {
  if (REXCVAR_GET(dante_audio_sin_compresor)) {
    static bool avisado = false;
    if (!avisado) {
      avisado = true;
      REXLOG_INFO("[audio] compresor final del juego desactivado (dante_audio_sin_compresor)");
    }
    ctx.r3.u64 = 1;  // what the bypass returns
    return;
  }
  __imp__rw__audio__core__Compressor1__Process(ctx, base);
}

// ---- Native versions of the mixer's leaf loops. Translated, each 4-float VMX vector goes through byte shuffles and
// stores in the context; MixWithGain alone runs 100-170 thousand times a second in a fight (about 14 % of a core at
// stock clocks). The same arithmetic on NEON: big-endian floats swapped on load and store, multiply then add (not
// fused), as vmaddfp.

REXCVAR_DEFINE_BOOL(dante_audio_nativo, true, "Dante",
                    "MixWithGain y CopyWithGain del mezclador en NEON nativo en vez de traducidos");

namespace {

#if defined(__aarch64__)
inline float32x4_t CargarBe(const uint8_t* p) { return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p))); }
inline void GuardarBe(uint8_t* p, float32x4_t v) { vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v))); }
#endif

inline float CargarFloatBe(const uint8_t* p) {
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

inline void GuardarFloatBe(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
}

// dst[i] = dst[i] + src[i] * gain (mezclar) or src[i] * gain.
void ConGanancia(uint8_t* dst, const uint8_t* src, float gain, uint32_t n, bool mezclar) {
  uint32_t i = 0;
#if defined(__aarch64__)
  const float32x4_t g = vdupq_n_f32(gain);
  for (; i + 8 <= n; i += 8) {
    const float32x4_t a = vmulq_f32(CargarBe(src + i * 4), g);
    const float32x4_t b = vmulq_f32(CargarBe(src + i * 4 + 16), g);
    if (mezclar) {
      GuardarBe(dst + i * 4, vaddq_f32(CargarBe(dst + i * 4), a));
      GuardarBe(dst + i * 4 + 16, vaddq_f32(CargarBe(dst + i * 4 + 16), b));
    } else {
      GuardarBe(dst + i * 4, a);
      GuardarBe(dst + i * 4 + 16, b);
    }
  }
#endif
  for (; i < n; ++i) {
    const float v = CargarFloatBe(src + i * 4) * gain;
    GuardarFloatBe(dst + i * 4, mezclar ? CargarFloatBe(dst + i * 4) + v : v);
  }
}

}  // namespace

// void MixWithGain(float* dst, float* src, float gain, unsigned count)
REX_EXTERN(__imp__rw__audio__core__MixWithGain);
REX_EXTERN(rw__audio__core__MixWithGain) {
  if (REXCVAR_GET(dante_audio_nativo)) {
    ConGanancia(REX_RAW_ADDR(ctx.r3.u32), REX_RAW_ADDR(ctx.r4.u32), float(ctx.f1.f64), ctx.r6.u32, true);
  } else {
    __imp__rw__audio__core__MixWithGain(ctx, base);
  }
}

// void CopyWithGain(float* dst, float* src, float gain, unsigned count)
REX_EXTERN(__imp__rw__audio__core__CopyWithGain);
REX_EXTERN(rw__audio__core__CopyWithGain) {
  if (REXCVAR_GET(dante_audio_nativo)) {
    ConGanancia(REX_RAW_ADDR(ctx.r3.u32), REX_RAW_ADDR(ctx.r4.u32), float(ctx.f1.f64), ctx.r6.u32, false);
    return;
  }
  __imp__rw__audio__core__CopyWithGain(ctx, base);
}
