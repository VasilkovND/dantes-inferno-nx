// dantes-inferno-nx - host setjmp/longjmp for the game's guest setjmp/longjmp.
//
// The PCH includes this header (when present) for code the generated functions call. The game saves and loads through
// a guest setjmp/longjmp pair (TU2: sub_82879110 / sub_82878CD0). Recompiled, a guest longjmp cannot unwind the host
// C++ frames of the recompiled calls between the two, so Hell's Gate's patches/generated/apply_generated_patches.py
// (run on the generated code by tools/build_mac.sh) rewrites each call site:
//   setjmp call:   g_setjmp_ctx_addr = r3; call the guest setjmp; then a host setjmp(g_fiber_jmp_buf), and when that
//                  returns again from a longjmp, FiberRestoreContext reloads the guest registers;
//   guest longjmp: FiberLongjmp(r3 or the value it returns) instead of its blr.
// These are the names and the behaviour that script expects. The buffer is per thread: setjmp and longjmp of one save
// run on one thread.
//
// Guest jmp_buf layout (what the guest setjmp stores): f14-f31 at 0, r1 at 144, r13-r31 at 152, CR at 304, LR at 308,
// v64-v127 at 320.

#pragma once

#include <rex/ppc.h>

#include <csetjmp>
#include <cstdint>
#include <cstring>

inline thread_local jmp_buf g_fiber_jmp_buf;
inline thread_local uint32_t g_setjmp_ctx_addr = 0;
inline thread_local uint32_t g_longjmp_return_value = 0;

// Inside the guest setjmp itself: remember its buffer and arm the host one. A macro, so the host setjmp runs in the
// recompiled function's own frame (GCC never inlines a function that calls setjmp). The call sites arm it again after
// the guest setjmp returns, in their frame, which is the one a longjmp comes back to.
#define FiberSetjmp(ctx_addr) (g_setjmp_ctx_addr = (ctx_addr), setjmp(g_fiber_jmp_buf))

[[noreturn]] inline void FiberLongjmp(uint32_t return_value) {
  g_longjmp_return_value = return_value;
  std::longjmp(g_fiber_jmp_buf, 1);
}

inline void FiberRestoreContext(PPCContext& ctx, uint8_t* base) {
  const uint32_t buffer = g_setjmp_ctx_addr;
  if (!buffer) return;
  // Guest addresses from 0xE0000000 are the 4 KB-page physical view, mapped 0x1000 higher in host memory.
  const uint8_t* p = base + buffer + (buffer >= 0xE0000000u ? 0x1000u : 0u);
  auto u64 = [p](uint32_t offset) {
    uint64_t v;
    std::memcpy(&v, p + offset, 8);
    return __builtin_bswap64(v);
  };
  auto u32 = [p](uint32_t offset) {
    uint32_t v;
    std::memcpy(&v, p + offset, 4);
    return __builtin_bswap32(v);
  };
  for (int i = 14; i <= 31; ++i) (&ctx.f0)[i].u64 = u64(uint32_t(i - 14) * 8);
  ctx.r1.u64 = u64(144);
  // The context keeps r3 first (r3, r0, r1, r2, r4, ...): from r4 on, rN is entry N counted from r3.
  for (int i = 13; i <= 31; ++i) (&ctx.r3)[i].u64 = u64(152 + uint32_t(i - 13) * 8);
  const uint32_t cr = u32(304);
  for (int i = 0; i < 8; ++i) (&ctx.cr0)[i].set_raw((cr >> (28 - i * 4)) & 0xF);
  ctx.lr = u32(308);
  // Vector registers are kept byte-reversed in the context.
  for (int i = 64; i <= 127; ++i) {
    const uint8_t* src = p + 320 + uint32_t(i - 64) * 16;
    uint8_t* dst = (&ctx.v0)[i].u8;
    for (int b = 0; b < 16; ++b) dst[b] = src[15 - b];
  }
  ctx.r3.u64 = g_longjmp_return_value;
}
