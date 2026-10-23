// dantes-inferno-nx - how the recompiled code's "interrupts off" regions behave (see tools/make_nx_pch.py, point 3).
//
// The game brackets its lwarx/stwcx. atomics with mtmsrd. The codegen made each region one process-wide mutex; the
// Switch PCH keeps that state per thread instead, which is enough for the atomics (host compare-and-swap) and lets the
// job scheduler's threads run side by side. dante_msr_cerrojo_global brings the old mutex back, for comparing.

#include "dantes_inferno_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <mutex>
#include <string>
#include <unordered_set>

#include "dante_cpu.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(dante_msr_cerrojo_global, false, "Dante",
                    "Las regiones con interrupciones apagadas del codigo recompilado (mtmsrd) como un mutex global "
                    "del proceso, como las genera el codegen. false = estado por hilo (los atomicos ya son CAS)");

namespace dante::cpu {

void Configurar() {
  ppc_msr_cerrojo_global_() = REXCVAR_GET(dante_msr_cerrojo_global);
  REXLOG_INFO("[cpu] regiones mtmsrd: {}", ppc_msr_cerrojo_global_() ? "mutex global (codegen)" : "estado por hilo");
}

}  // namespace dante::cpu

namespace dante::cpu {

void InformarSistema() {
#if defined(__SWITCH__)
  const u32 hos = hosversionGet();
  const bool aplicacion = appletGetAppletType() == AppletType_Application;
  REXLOG_INFO("dantes-inferno-nx {} (commit {}), firmware {}.{}.{}{}, {}, {}", DANTE_VERSION, DANTE_COMMIT, HOSVER_MAJOR(hos),
              HOSVER_MINOR(hos), HOSVER_MICRO(hos), hosversionIsAtmosphere() ? " (Atmosphere)" : "",
              aplicacion ? "title takeover" : "APPLET MODE",
              appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");
  if (!aplicacion) {
    REXLOG_WARN("dantes-inferno-nx: started as an applet (from the album?): not enough memory for the game. Start any game while "
                "holding R and open the Homebrew Menu from there");
  }
#else
  REXLOG_INFO("dantes-inferno-nx {} (commit {})", DANTE_VERSION, DANTE_COMMIT);
#endif
}

}  // namespace dante::cpu

// The check build of tools/patch_local_regs.py (PLR_CHECK=1): a call changed registers the analysis said it keeps.
// Logged once per call site (its number, dante/out/codegen/patch_local_regs.check.map), at most 256 sites, with each
// register that changed (before -> after). A poisoned value a callee read shows up as a fault at 0x5Exxxxxx instead:
// the low 24 bits are the site's number too.
extern "C" void dante_regcheck_fail(const char* function, const char* callee, int site, const char* regs,
                                    const uint64_t* before, const PPCContext* ctx) {
  static std::mutex mutex;
  static std::unordered_set<int> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 256 || !seen.insert(site).second) return;
  }
  std::string changed;
  const char* p = regs;
  for (int k = 0; *p; ++k) {
    const char kind = *p++;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    while (*p == ' ') ++p;
    uint64_t now = 0;
    if (kind == 'r') {
      static const PPCRegister PPCContext::*const kR[32] = {
          &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
          &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
          &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
          &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
          &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
          &PPCContext::r30, &PPCContext::r31};
      now = (ctx->*kR[n & 31]).u64;
    } else {
      static const PPCRegister PPCContext::*const kF[32] = {
          &PPCContext::f0,  &PPCContext::f1,  &PPCContext::f2,  &PPCContext::f3,  &PPCContext::f4,  &PPCContext::f5,
          &PPCContext::f6,  &PPCContext::f7,  &PPCContext::f8,  &PPCContext::f9,  &PPCContext::f10, &PPCContext::f11,
          &PPCContext::f12, &PPCContext::f13, &PPCContext::f14, &PPCContext::f15, &PPCContext::f16, &PPCContext::f17,
          &PPCContext::f18, &PPCContext::f19, &PPCContext::f20, &PPCContext::f21, &PPCContext::f22, &PPCContext::f23,
          &PPCContext::f24, &PPCContext::f25, &PPCContext::f26, &PPCContext::f27, &PPCContext::f28, &PPCContext::f29,
          &PPCContext::f30, &PPCContext::f31};
      now = (ctx->*kF[n & 31]).u64;
    }
    if (now != before[k]) changed += fmt::format(" {}{} {:X}->{:X}", kind, n, before[k], now);
  }
  REXLOG_WARN("[regcheck] site {:06X} ({}): the call to {} changed{}", site, function, callee, changed);
}
