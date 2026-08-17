/* kernel/arch/riscv64/sbi.c -- SBI ecall wrappers (RISCV_PLAN V0).
 *
 * One inline-asm site, deliberately: every SBI call funnels through
 * sbi_call() so the V6 sweep has a single audited ecall in this file
 * rather than one per wrapper.  Register mapping per the SBI spec:
 * a7 = EID, a6 = FID, a0..a5 = arguments, returns in a0 (error) and
 * a1 (value).
 */

#include <stdint.h>

#include "kernel/arch/riscv64/sbi.h"

#define SBI_EID_BASE       0x10
#define SBI_FID_SPEC_VER   0
#define SBI_FID_IMPL_ID    1
#define SBI_FID_PROBE_EXT  3

#define SBI_EID_LEGACY_PUTCHAR  0x01
#define SBI_EID_LEGACY_GETCHAR  0x02
#define SBI_EID_LEGACY_SHUTDOWN 0x08

#define SBI_EID_DBCN       0x4442434E   /* "DBCN" */
#define SBI_FID_DBCN_WRITE 0

static struct sbiret sbi_call(long eid, long fid,
                              long a0, long a1, long a2)
{
    register long r_a0 __asm__("a0") = a0;
    register long r_a1 __asm__("a1") = a1;
    register long r_a2 __asm__("a2") = a2;
    register long r_a6 __asm__("a6") = fid;
    register long r_a7 __asm__("a7") = eid;

    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a6), "r"(r_a7)
                     : "memory");

    return (struct sbiret){ .error = r_a0, .value = r_a1 };
}

struct sbiret sbi_get_spec_version(void)
{
    return sbi_call(SBI_EID_BASE, SBI_FID_SPEC_VER, 0, 0, 0);
}

struct sbiret sbi_get_impl_id(void)
{
    return sbi_call(SBI_EID_BASE, SBI_FID_IMPL_ID, 0, 0, 0);
}

struct sbiret sbi_probe_extension(long eid)
{
    return sbi_call(SBI_EID_BASE, SBI_FID_PROBE_EXT, eid, 0, 0);
}

/* ---- console ------------------------------------------------------------ */

static int have_dbcn;   /* set once by sbi_console_init */

void sbi_console_init(void)
{
    struct sbiret r = sbi_probe_extension(SBI_EID_DBCN);
    have_dbcn = (r.error == 0 && r.value != 0);
}

void sbi_putc(char c)
{
    if (have_dbcn) {
        /* DBCN write: num_bytes, base_addr_lo, base_addr_hi -- and
         * the address must be PHYSICAL (the SBI spec says so; OpenSBI
         * reads the buffer from M-mode with paging off).  Since V3
         * the stack is a higher-half VA, so the HHDM offset comes
         * back off before the pointer crosses to the firmware.
         * (V0..V2 ran satp=0 where VA==PA and the subtraction would
         * have been wrong; this line is version-locked to the boot
         * layout in boot.S, which turns Sv39 on before any C runs.) */
        /* STATIC, not a stack local -- measured in V4: the trap
         * handler's stack is a kheap allocation at 0xFFFFFFE0...,
         * where VA - HHDM is NOT the physical address; OpenSBI
         * faulted loading from the garbage "physical" 0x2000001eaf.
         * A static lives in .bss, whose VA is HHDM + phys by the
         * linker map, so the subtraction is exact.  Single hart
         * writes console (D5), so one byte of state races nothing. */
        static char buf;
        buf = c;
        sbi_call(SBI_EID_DBCN, SBI_FID_DBCN_WRITE,
                 1, (long)((uintptr_t)&buf - 0xFFFFFFC000000000UL), 0);
    } else {
        sbi_call(SBI_EID_LEGACY_PUTCHAR, 0, (long)c, 0, 0);
    }
}

void sbi_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            sbi_putc('\r');
        sbi_putc(*s++);
    }
}

int sbi_getchar(void)
{
    /* Legacy getchar returns the byte (or -1) in a0 -- the ERROR slot
     * of the modern convention; this call predates the split. */
    struct sbiret r = sbi_call(SBI_EID_LEGACY_GETCHAR, 0, 0, 0, 0);
    return (int)r.error;
}

/* ---- timer (V2) ---------------------------------------------------------- */

#define SBI_EID_TIME             0x54494D45   /* "TIME" */
#define SBI_FID_TIME_SET         0
#define SBI_EID_LEGACY_SET_TIMER 0x00

static int have_time_ext = -1;   /* -1 = not probed yet */

void sbi_set_timer(uint64_t stime_value)
{
    if (have_time_ext < 0) {
        struct sbiret r = sbi_probe_extension(SBI_EID_TIME);
        have_time_ext = (r.error == 0 && r.value != 0);
    }
    if (have_time_ext)
        sbi_call(SBI_EID_TIME, SBI_FID_TIME_SET,
                 (long)stime_value, 0, 0);
    else
        sbi_call(SBI_EID_LEGACY_SET_TIMER, 0, (long)stime_value, 0, 0);
}

void sbi_shutdown(void)
{
    sbi_call(SBI_EID_LEGACY_SHUTDOWN, 0, 0, 0, 0);
    for (;;)
        __asm__ volatile("wfi");
}
