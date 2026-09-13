/* w32_teb.h — the W32A-3 TEB-lite: per-thread state reached through %gs.
 *
 * One 4 KiB page per thread, installed as the thread's USER GS.base via
 * ARCH_SET_GS (the main thread at w32_thr_init, every other thread in its
 * CreateThread trampoline before user code runs).  The kernel's swapgs
 * protocol (see the audit note in kernel/arch/x86_64/isr_stubs.asm) makes
 * this page live exactly while Ring 3 runs.
 *
 * OFFSET PROVENANCE (read before "fixing" a number): only ONE offset in
 * this struct is constrained from the outside — ThreadLocalStoragePointer
 * at +0x58, which W32APP_PLAN.md names as the GS:[0x58]-family contract
 * the ladder's CRT uses for __declspec(thread) variables (TEB+0x58 points
 * at the per-module block array; the compiler indexes it by the module's
 * TLS slot).  Every other offset is an AuraLite choice, pinned by
 * static_assert below and by the guest fixtures, and W32A-13 (msvcrt)
 * re-validates the whole layout against the real CRT's raw GS reads when
 * that phase measures them.  Nothing here is copied from memory of the
 * Windows TEB: resemblance of +0x30 (Self) and +0x58 to the real thing is
 * deliberate compatibility surface, documented as such.
 *
 * FS is NOT this struct: libc owns FS (errno at %fs:8, see the FIX_R3 note
 * in lib/libc/include/pthread_tls.h).  w32 threads carry a separate FS
 * cell (struct w32_fscell in kernel32_thr.c) whose first fields mirror the
 * pthread TCB so libc errno keeps working; that coupling is ABI, pinned by
 * numeric static_asserts there.
 */

#ifndef AURALITE_W32_TEB_H
#define AURALITE_W32_TEB_H

#include "w32/w32_abi.h"

#include <stddef.h>
#include <stdint.h>

/* Slot counts.  64 TLS slots and 128 FLS slots are the Win32 minima a
 * program may rely on (TLS_MINIMUM_AVAILABLE / FLS_MAXIMUM_AVAILABLE);
 * the ladder never needs more, and the page budget fits exactly these. */
#define W32_TEB_TLS_SLOTS 64
#define W32_TEB_FLS_SLOTS 128

/* Loader-owned per-module TLS slot count: the exe plus this many DLLs can
 * carry implicit (__declspec(thread)) TLS at once.  Slot 0 is always the
 * main image.  The ladder's heaviest image uses a handful; 16 is headroom
 * with a loud refusal (not silent truncation) past it. */
#define W32_TLS_MODULES_MAX 16

struct w32_teb {
    uint64_t nt_exception_list;   /* +0x00: reserved, always 0 (no SEH chain) */
    uint64_t nt_stack_base;       /* +0x08: thread stack top (highest addr) */
    uint64_t nt_stack_limit;      /* +0x10: thread stack bottom (lowest addr) */
    uint64_t nt_subsystem_tib;    /* +0x18: reserved, always 0 */
    uint64_t nt_fiber_data;       /* +0x20: reserved (fibers: none, honest) */
    uint64_t nt_arbitrary;        /* +0x28: reserved, always 0 */
    uint64_t self;                /* +0x30: this TEB's own address */
    uint32_t client_pid;          /* +0x38: process id (main thread's tid) */
    uint32_t client_tid;          /* +0x3c: this thread's tid */
    void    *thread_obj;          /* +0x40: w32-internal struct w32_thread* */
    uint32_t last_error;          /* +0x48: LastError (was process-global) */
    uint32_t pad_4c;
    uint64_t pad_50;              /* +0x50: reserved */
    uint64_t tls_storage_ptr;     /* +0x58: ThreadLocalStoragePointer */
    void    *tls_slots[W32_TEB_TLS_SLOTS]; /* +0x60: TlsAlloc slots */
    void    *fls_slots[W32_TEB_FLS_SLOTS]; /* +0x260: FlsAlloc slots */
};

/* The layout contract, pinned at compile time.  The guest fixtures pin it
 * again at run time with raw %gs reads. */
_Static_assert(offsetof(struct w32_teb, self) == 0x30, "TEB Self");
_Static_assert(offsetof(struct w32_teb, tls_storage_ptr) == 0x58,
               "TEB ThreadLocalStoragePointer");
_Static_assert(offsetof(struct w32_teb, tls_slots) == 0x60, "TEB TlsSlots");
_Static_assert(offsetof(struct w32_teb, fls_slots) == 0x260, "TEB FlsSlots");
_Static_assert(sizeof(struct w32_teb) <= 4096, "TEB fits one page");

/* The current thread's TEB, or NULL before w32_thr_init ran.  Valid in any
 * thread running w32 code: the main TEB is installed by process init and
 * every worker's by its trampoline before user code.  Reads %gs:0x30; the
 * ready flag (not GS itself) decides NULL, because probing an
 * uninstalled GS would fault instead of answering. */
struct w32_teb *w32_teb_self(void);

/* Process init: install the main thread's TEB + ARCH_SET_GS, cache the
 * pid, zero the slot allocators.  Idempotent (the host suite re-inits).
 * Runs before any other w32 call, from w32_kernel32_init. */
void w32_thr_init(void);
void w32_thr_checkpoint(void);

/* 1 once w32_thr_init has run (the w32_teb_self NULL gate). */
int w32_thr_ready(void);

#endif /* AURALITE_W32_TEB_H */
