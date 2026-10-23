# CPU optimization of the recompiled code

The CPU passes of gears2-nx 1.0.0 (Gears of War 2's port, same ReXGlue v0.10.0 codegen and SDK), carried over to this
game. Their design, the whole-program analysis (REF / MOD / PRES / CH / LIVEOUT), the pitfalls met on the way and the
ideas left are written up in gears2-nx's `gow2/CPU_OPTIMIZATION.md`; this file only says what differs here.

## Where the time goes

The game's `Presentation` thread runs its main loop and records the frame: 85-99 % of a core in every measured session,
0 % of it waiting in the kernel (stack sampler, run 4), with a flat profile. At stock clocks (CPU 1020 MHz) gameplay
drops to 37-50 fps with that thread at 80-88 %; the GPU takes 13-22 ms a frame there, so heavy scenes are partly GPU-bound
too (section 6 of PORTING.md). Overclocked (CPU 1963 MHz, GPU 614 MHz) it holds 60 fps except for scenes where the GPU
takes more than 16.7 ms (they fall to 30 with vsync).

## The passes (`tools/codegen.sh`, step 5)

1. `tools/patch_stack_access.py`: guest stack accesses (`ctx.r1.u32 + k`, and r12 in the save/restore helpers) through
   `REX_SLOAD_*` / `REX_SSTORE_*` (memcpy, `make_nx_pch.py` point 4) instead of volatile pointers.
2. `tools/patch_inline_calls.py`: calls to a function in the same file that no hook replaces go to its body
   (`__imp__<name>`), which GCC may inline; the register save/restore helpers (`__savegprlr_N` ...) are copied into
   `dantes_inferno_inline_helpers.h` and inlined at every call.
3. `tools/patch_local_regs.py`: the guest registers in host locals inside each function, ctx exact only at calls and
   returns for the registers that matter. `LOCALREGS=0` skips it; `PLR_CHECK=1` builds the check version.

What is different from gears2-nx:

- **Hooks by name.** This port replaces a guest function by defining a strong symbol of its name
  (`REX_EXTERN(VP6_DecodeFrameMbs) {`, `DANTE_SKIN(SkinVerts4, 4)`) and calls the original as `__imp__<name>`. So
  "hooked" is any generated function named in `dante/src` or the SDK outside comments and the SDK's `codegen/` folders
  (today the 9 real ones: the two shader creations, SkinVerts1/3/4, VP6_DecodeFrameMbs and the three audio functions).
  They keep their calls through the alias, are taken at the calling convention's word at their call sites, and get
  LIVEOUT = everything. The game's own named functions (`VP6_*`, `unresolved_target_*`) are analysed like `sub_`.
- **No stack guard.** This port's `REX_FUNC_PROLOGUE` is only the alignment hint, so `GUARD_REF/GUARD_MOD` are gone.
- **Guest setjmp/longjmp** (`src/dantes_inferno_hooks.h`, Hell's Gate's `apply_generated_patches.py`). Where a host
  `setjmp` comes back from a longjmp, `FiberRestoreContext(ctx, base)` loads the registers the guest setjmp saved into
  ctx. The pass takes that line as a call that reads nothing and may change everything (MOD = all), and reloads every
  local of the function after it. The guest setjmp itself (sub_82879110) reads the non-volatiles it stores from ctx,
  so its REF makes callers write them back first. Without this every such function was "another use of ctx", and one
  such function turns LIVEOUT off for the whole program.
- The check build reports through `dante_regcheck_fail` (`src/dante_cpu.cpp`); its sites are listed in
  `dante/out/codegen/patch_local_regs.check.map`.
- `tools/localize_regs.py` (an earlier, unused copy of a few VP6 functions with their registers in locals, writing
  everything back at every call) is gone.

Numbers today: all 36,715 functions rewritten, none left as they were; 826 same-file calls may be inlined and 24,555
helper calls are; `dantes_inferno_recomp.64.cpp` 275 -> 219 KB of code.

## The check build

`PLR_CHECK=1 dante/tools/build_mac.sh <game folder>`: before each call the dirty registers the callee should not read
are poisoned with `0x5EAD5EAD_5E<site>`, and the live ones it should keep are compared after it. Play 5-10 minutes
(menus, a video, a fight, a save and a load: the save goes through the guest setjmp), then look for `[regcheck]` in the
game log and for faults at `0x5Exxxxxx` in `rex_crash.log`. Build it after any change to the passes, before measuring.

## Measuring

`logs/rex/rex_perfil.log`: clocks (`relojes:`), `juego N fps` and each thread's CPU % per 10 s; the game log's
`[deko3d] ultimos 10 s: N fotogramas/s, D dibujos por fotograma ... GPU M ms por fotograma`. Compare the same place at
the same clocks, sampler off.
