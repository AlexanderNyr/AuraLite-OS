# AuraLite OS — POSIX.1-2024 Compliance Plan

## Status: IN PROGRESS 🔧 (Q1 complete)

This document is the living development plan for POSIX.1-2024 (IEEE Std
1003.1-2024, The Open Group Base Specifications Issue 8) compliance in
AuraLite OS. It follows the same structure as `POSIX_PLAN.md` (the
POSIX.1-2017 roadmap, P1-P10, now complete) and `PLAN.md` (the original
14-phase roadmap).

Baseline: commit `34bde09` (2026-07-05). All P1-P10 POSIX.1-2017 work is
complete and is **not** revisited here except where POSIX.1-2024 changes an
existing header's contents.

The plan is divided into 12 phases (Q1-Q12), ordered by dependency and
impact. Q1-Q4 have no dependencies between them; Q5+ depend on Q1.

---

## Phase Q1 — Mandatory C Standard Headers (Thin Wrappers)

**Objective:** Add the C standard headers that POSIX.1-2024 mandates but
were missing from the P1-P10 baseline.

### Status: DONE ✅

### What was added

**Headers** (`libc/include/`):

| Header | Approach |
|---|---|
| `stdarg.h` | Thin wrapper over `__builtin_va_*` |
| `stddef.h` | `#include_next` the compiler's freestanding header (+ `max_align_t` fallback) |
| `stdint.h` | `#include_next` the compiler's freestanding header |
| `float.h` | Hand-written IEEE 754 binary32/64 + x87 80-bit extended constants |
| `inttypes.h` | Format macros for AuraLite's actual LP64 typedefs (`int64_t`/`intmax_t` are `long`, **not** `long long`, on this target — verified with both Clang and GCC) + `strtoimax`/`strtoumax`/`imaxabs`/`imaxdiv` |
| `iso646.h` | Alternative operator spellings |
| `stdalign.h` | `alignas`/`alignof` macros (guarded for C23+ toolchains where they're keywords) |
| `stdnoreturn.h` | `noreturn` macro (guarded for C23+) |
| `tgmath.h` | Type-generic macros; dispatches to the `double`-only functions AuraLite's `<math.h>` provides (no `float`/`long double` overloads exist yet) |
| `complex.h` | Stub: types/macros compile, `creal`/`cimag`/`cabs`/`conj` implemented, no real complex arithmetic |
| `fenv.h` | Stub: AuraLite never establishes a per-thread FP environment, so all functions report the fixed default environment and never fail |
| `stdatomic.h` | Full C11 atomics on top of compiler builtins (`__c11_atomic_*` for Clang, `__atomic_*` for GCC — see header comment for why both are needed) |
| `wctype.h` | "C"-locale wide-char classification (ASCII widened to `wint_t`) |
| `strings.h` | BSD compatibility (`bcmp`/`bcopy`/`bzero`/`index`/`rindex`/`ffs*`) |
| `uchar.h` | `char16_t`/`char32_t` + UTF-8 single-code-point conversions (ASCII exact, `EILSEQ` beyond ASCII — full UTF-8 decoding is future work) |
| `setjmp.h` | Types + prototypes; implementation in `libc/crt/setjmp.asm` and `libc/src/compat.c` |
| `threads.h` | C11 threads mapped onto `libc/src/pthread/pthread.c` |

**Runtime:**
- `libc/crt/setjmp.asm` — x86_64 `setjmp`/`longjmp` (System V AMD64 ABI,
  callee-saved registers: rbx/rbp/r12-r15/rsp/rip).
- `libc/src/compat.c` — runtime bodies for the above headers' non-builtin
  functions (BSD strings.h aliases, wctype.h C-locale classification,
  inttypes.h helpers, threads.h-over-pthreads wrappers, fenv.h/complex.h
  stubs, `sigsetjmp`/`siglongjmp`, uchar.h conversions).
- `Makefile`: `setjmp.o` and `compat.o` added to `USER_COMMON`, linked into
  every user-space ELF (same treatment as `malloc.o`/`pthread.o`/etc.).

**Bug found and fixed along the way:** adding `<stdnoreturn.h>` exposed a
latent macro-hygiene bug in four existing headers (`assert.h`, `setjmp.h`,
`stdlib.h`, `threads.h`): `__attribute__((noreturn))` expands to
`__attribute__((_Noreturn))` once the `noreturn` macro is defined, which
GCC rejects under `-Werror` (`'_Noreturn' attribute directive ignored`).
Fixed by switching all four declarations to the double-underscore attribute
spelling `__attribute__((__noreturn__))`, which is immune to macro expansion
per both compilers' documented behaviour.

**Tests:**
- `tests/unit/test_q1_headers.c` (new, registered in `make test-unit`):
  includes all 17 new/changed headers together under
  `-std=c11 -Wall -Wextra -Werror`, and exercises the non-builtin logic
  (inttypes helpers, iso646 operators, stdalign/stdnoreturn, wctype
  classification, `strings.h` `ffs`, `jmp_buf`/`sigjmp_buf` layout,
  `stdatomic.h` load/store/fetch-add/flag, fenv defaults, complex stub
  types) via standalone reimplementations — mirroring how the existing
  `test_signals.c`/`test_ctype.c` host tests avoid linking the freestanding
  syscall-backed runtime.

### Definition of Done
- [x] All 17 touched/new headers `#include` cleanly, standalone and
      together, under both host GCC and cross Clang (`-Wall -Wextra
      -Werror`).
- [x] `libc/src/compat.c` compiles cleanly under both compilers and links
      into every user-space ELF.
- [x] `libc/crt/setjmp.asm` assembles with `nasm` and is linked into every
      user-space ELF.
- [x] `make test-unit` passes `test_q1_headers` (42/42 checks) with **zero
      regressions** in the other 27 existing unit-test binaries.
- [x] `make all` (full ISO + kernel.elf + every user-space app) builds with
      zero errors/warnings, unchanged from baseline.
- [x] No kernel-side changes: the kernel's `CFLAGS` do not include
      `libc/include`, so Q1 is verified to be a pure user-space/libc change
      with no kernel build-path risk, matching the plan's "zero kernel risk"
      characterization of this phase.

---

## Phases Q2-Q12

Not started in this pass. See the original planning brief for the full
task breakdown (stdio/string/stdlib extensions, AT-family syscalls, pthread
extensions, POSIX IPC, sched/resource, `posix_spawn`, locale/iconv/search
stubs, POSIX.1-2024-new functions, and the final compliance-matrix/test-suite
phase). Recommended order (unchanged from the plan): Q1 (done) -> Q3 -> Q2
-> Q4 -> Q5 -> Q11 -> Q6 -> Q8 -> Q7 -> Q9 -> Q10 -> Q12.
