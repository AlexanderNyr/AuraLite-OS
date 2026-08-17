/* kernel/arch/riscv64/sbi.h -- Supervisor Binary Interface calls
 * (RISCV_PLAN V0, decision D2).
 *
 * The SBI is the S-mode kernel's contract with the resident M-mode
 * firmware (OpenSBI on QEMU virt and on every mainstream board) -- a
 * frozen public specification, used the way BIOS Stage 2 used INT 13h:
 * a platform service, not a code dependency.
 *
 * V0 scope: the console (output before any UART driver exists) and
 * the probe machinery.  The legacy console extension (EID 0x01) is
 * deprecated but universally present; the Debug Console extension
 * (DBCN, EID 0x4442434E, "DBCN") is its replacement.  V0 probes DBCN
 * via sbi_probe_extension and falls back -- OpenSBI version drift is
 * a named risk in the plan, and probing is the mitigation.
 */

#ifndef AURALITE_ARCH_RISCV64_SBI_H
#define AURALITE_ARCH_RISCV64_SBI_H

#include <stdint.h>

/* SBI return convention: a0 = error (0 = success), a1 = value. */
struct sbiret {
    long error;
    long value;
};

/* Base extension (EID 0x10): spec version, impl id, extension probe. */
struct sbiret sbi_get_spec_version(void);
struct sbiret sbi_get_impl_id(void);
struct sbiret sbi_probe_extension(long eid);

/* Console: DBCN when the firmware has it, legacy putchar otherwise.
 * sbi_console_init() picks once at boot and reports which. */
void sbi_console_init(void);
void sbi_putc(char c);
void sbi_puts(const char *s);

/* Non-blocking console read: the pending byte, or -1 when none.
 * Legacy EID 0x02 -- the return travels in the ERROR slot by that
 * extension's convention (it predates the error/value split).  V5's
 * cooked-line reader polls this; V7's UART driver replaces it. */
int  sbi_getchar(void);

/* TIME extension (EID 0x54494D45, "TIME"): arm the next timer
 * interrupt at an absolute timebase value.  One-shot -- the trap
 * handler re-arms.  Falls back to legacy set_timer (EID 0x00) if the
 * firmware predates the extension (same probe-once pattern as the
 * console). */
void sbi_set_timer(uint64_t stime_value);

/* Legacy shutdown (EID 0x08) -- lets the smoke tests end a run without
 * waiting for the QEMU timeout, the way -no-reboot + hlt does on x86. */
void sbi_shutdown(void) __attribute__((noreturn));

#endif /* AURALITE_ARCH_RISCV64_SBI_H */
