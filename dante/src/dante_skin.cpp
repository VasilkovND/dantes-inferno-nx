// dantes-inferno-nx - the game's vertex skinning on the CPU, native (dante_skin_impl.h).
//
// Dante's Inferno skins its characters on the CPU: three VMX loops (4, 3 and 1 bones per vertex) on the Presentation
// thread and the EARS job workers, about an eighth of the Presentation thread at stock clocks (run 5's profile, the
// thread that bounds the frame rate there). Recompiled, every one of their vector registers lives in the PPCContext and
// every output vertex is stored a byte at a time; the native versions do the same vector operations in registers, with
// the same results bit for bit (dante/tools/skin_test.py checks it on the Mac against the recompiled code: about three
// times faster there). dante_skin_nativo=false runs the recompiled loops.

#include "dante_skin_impl.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(dante_skin_nativo, true, "Dante",
                    "El skinning de vertices del juego (SkinVerts1/3/4) nativo en vez de recompilado");

#define DANTE_SKIN(nombre, huesos)                       \
  REX_EXTERN(__imp__##nombre);                           \
  REX_EXTERN(nombre) {                                   \
    if (!REXCVAR_GET(dante_skin_nativo) || !ctx.r4.u32) { \
      __imp__##nombre(ctx, base);                        \
      return;                                            \
    }                                                    \
    dante::skin::SkinVerts<huesos>(ctx, base);           \
  }

DANTE_SKIN(SkinVerts4, 4)
DANTE_SKIN(SkinVerts3, 3)
DANTE_SKIN(SkinVerts1, 1)
