/* bsod.c — fatal stop screen and the STOP-code table.
 *
 * Serial dump (diag_early_*) is the lock-free record; this file is the
 * human-facing screen.  It must not take the kprintf lock and must not
 * allocate.  The compositor is seized first so an AP cannot flip the
 * blue screen away.
 */

#include "kernel/lib/bsod.h"
#include <stddef.h>

#ifndef AURALITE_BSOD_HOST_TEST
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/graphics.h"
#endif

/* Lock-free serial — implemented in diagnostics.c.  Declared here so
 * this file does not include an arch header (width-sweep ratchet 2). */
void diag_early_puts(const char *s);
void diag_early_puthex(uint64_t v);
void diag_early_putdec(uint64_t v);

#ifdef AURALITE_BSOD_HOST_TEST
#include <stdio.h>
#include <string.h>
void diag_early_puts(const char *s) { fputs(s ? s : "", stdout); }
void diag_early_puthex(uint64_t v)  { printf("0x%016llx", (unsigned long long)v); }
void diag_early_putdec(uint64_t v)  { printf("%llu", (unsigned long long)v); }
void fb_bsod_paint(const char *const *lines, int nlines)
    { (void)lines; (void)nlines; }
void gfx_bsod_seize(void) {}
#endif

struct bsod_entry {
    uint32_t    stop;
    const char *name;
    const char *meaning;
};

/* CPU exception names match Intel SDM Vol.3 §6.15.  Software stops
 * are AuraLite's own.  Keep this table the source of truth for
 * docs/bsod.md — the host unit test walks it. */
static const struct bsod_entry k_stops[] = {
    { 0x00000000u, "DIVIDE_ERROR",
      "Integer division by zero or overflow (#DE)." },
    { 0x00000001u, "DEBUG",
      "Debug exception (#DB): breakpoint or single-step in the kernel." },
    { 0x00000002u, "NMI",
      "Non-maskable interrupt. Hardware reported a serious fault." },
    { 0x00000003u, "BREAKPOINT",
      "INT3 breakpoint (#BP) taken in kernel mode." },
    { 0x00000004u, "OVERFLOW",
      "INTO overflow (#OF) in the kernel." },
    { 0x00000005u, "BOUND_RANGE",
      "BOUND range exceeded (#BR)." },
    { 0x00000006u, "INVALID_OPCODE",
      "The CPU decoded an illegal instruction (#UD)." },
    { 0x00000007u, "DEVICE_NOT_AVAILABLE",
      "FPU/SSE used before it was enabled (#NM)." },
    { 0x00000008u, "DOUBLE_FAULT",
      "An exception hit while delivering another exception (#DF). Running on IST1." },
    { 0x00000009u, "COPROCESSOR_SEGMENT",
      "Coprocessor segment overrun (legacy, unused on long mode)." },
    { 0x0000000Au, "INVALID_TSS",
      "Invalid TSS referenced during a privilege change (#TS)." },
    { 0x0000000Bu, "SEGMENT_NOT_PRESENT",
      "A segment descriptor marked not-present was loaded (#NP)." },
    { 0x0000000Cu, "STACK_FAULT",
      "Stack-segment fault (#SS): a stack reference was not canonical or not present." },
    { 0x0000000Du, "GENERAL_PROTECTION",
      "General protection fault (#GP): a privileged or non-canonical access." },
    { 0x0000000Eu, "PAGE_FAULT",
      "Page fault (#PF). EXTRA is CR2, the address that was not mapped or not writable." },
    { 0x00000010u, "X87_FLOAT",
      "x87 floating-point exception (#MF)." },
    { 0x00000011u, "ALIGNMENT_CHECK",
      "Alignment check (#AC): a misaligned access with AC enabled." },
    { 0x00000012u, "MACHINE_CHECK",
      "Machine check (#MC): the CPU reported an uncorrectable hardware error." },
    { 0x00000013u, "SIMD_FLOAT",
      "SIMD floating-point exception (#XM)." },
    { 0x00000014u, "VIRTUALIZATION",
      "Virtualization exception (#VE)." },
    { 0x00000015u, "CONTROL_PROTECTION",
      "Control-protection exception (#CP, CET)." },
    { BSOD_KASSERT,  "KASSERT",
      "A kernel ASSERT() failed. The condition is printed as DETAIL." },
    { BSOD_KEXPLICIT, "KEXPLICIT",
      "An explicit kernel stop was requested. The message is printed as DETAIL." },
    { BSOD_KCANARY,  "KCANARY",
      "The stack protector tripped: a kernel stack cookie did not match." },
    { BSOD_KSTACK,   "KSTACK",
      "A kernel thread overflowed its stack onto the unmapped guard page." },
    { BSOD_KRECURSE, "KRECURSE",
      "A second fault occurred while printing the first diagnostic." },
    { BSOD_KHALT,    "KHALT",
      "kernel_halt() was reached without a more specific stop code." },
};

static const char k_unknown_name[]    = "UNKNOWN";
static const char k_unknown_meaning[] = "Unlisted stop code. See the serial dump and docs/bsod.md.";

const char *bsod_stop_name(uint32_t stop) {
    for (unsigned i = 0; i < (unsigned)(sizeof k_stops / sizeof k_stops[0]); i++) {
        if (k_stops[i].stop == stop) return k_stops[i].name;
    }
    return k_unknown_name;
}

const char *bsod_stop_meaning(uint32_t stop) {
    for (unsigned i = 0; i < (unsigned)(sizeof k_stops / sizeof k_stops[0]); i++) {
        if (k_stops[i].stop == stop) return k_stops[i].meaning;
    }
    return k_unknown_meaning;
}

#ifdef AURALITE_BSOD_HOST_TEST
/* Host tests only need the table.  The rest of this file is the
 * in-guest painter; skip it so we do not pull kernel headers. */

/* Existing QEMU gates grep the serial log for the substring PANIC
 * (ASSERT/PANIC macros print that tag; the sysrq crash paths must not).
 * Keep every painted name and meaning free of that substring. */
int bsod_table_has_panic_substring(void) {
    for (unsigned i = 0; i < (unsigned)(sizeof k_stops / sizeof k_stops[0]); i++) {
        if (strstr(k_stops[i].name, "PANIC") || strstr(k_stops[i].meaning, "PANIC"))
            return 1;
    }
    return 0;
}
#else

static volatile int bsod_shown;

static void hex32(char *out, uint32_t v) {
    static const char d[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++)
        out[2 + i] = d[(v >> (28 - 4 * i)) & 0xFu];
    out[10] = 0;
}

static void hex64(char *out, uint64_t v) {
    static const char d[] = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = d[(v >> (60 - 4 * i)) & 0xFu];
    out[18] = 0;
}

static void cpy(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (!src) src = "";
    while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
    dst[n] = 0;
}

static void cat(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (dst[n] && n + 1 < cap) n++;
    cpy(dst + n, cap - n, src);
}

void bsod_show(uint32_t stop, const char *detail,
               uint32_t cpu, uint64_t rip, uint64_t extra) {
    if (bsod_shown) return;
    bsod_shown = 1;

    const char *name = bsod_stop_name(stop);
    const char *mean = bsod_stop_meaning(stop);

    /* Serial first: lock-free, and the existing panic tests grep it. */
    diag_early_puts("\n[bsod] STOP=");
    diag_early_puthex(stop);
    diag_early_puts(" ");
    diag_early_puts(name);
    diag_early_puts("\n[bsod] ");
    diag_early_puts(mean);
    diag_early_puts("\n[bsod] cpu=");
    diag_early_putdec(cpu);
    diag_early_puts(" rip=");
    diag_early_puthex(rip);
    diag_early_puts(" extra=");
    diag_early_puthex(extra);
    diag_early_puts("\n[bsod] see docs/bsod.md\n");
    if (detail && detail[0]) {
        diag_early_puts("[bsod] detail=");
        diag_early_puts(detail);
        diag_early_puts("\n");
    }

    char stop_line[80];
    char rip_line[48];
    char extra_line[48];
    char cpu_line[32];
    char hex[20];

    cpy(stop_line, sizeof stop_line, "STOP: ");
    hex32(hex, stop);
    cat(stop_line, sizeof stop_line, hex);
    cat(stop_line, sizeof stop_line, "  (");
    cat(stop_line, sizeof stop_line, name);
    cat(stop_line, sizeof stop_line, ")");

    cpy(rip_line, sizeof rip_line, "RIP    ");
    hex64(hex, rip);
    cat(rip_line, sizeof rip_line, hex);

    cpy(extra_line, sizeof extra_line, "EXTRA  ");
    hex64(hex, extra);
    cat(extra_line, sizeof extra_line, hex);

    cpy(cpu_line, sizeof cpu_line, "CPU    ");
    /* cpu is small; one or two digits. */
    {
        size_t n = 0;
        while (cpu_line[n]) n++;
        if (cpu >= 10 && n + 2 < sizeof cpu_line) {
            cpu_line[n++] = (char)('0' + (cpu / 10) % 10);
            cpu_line[n++] = (char)('0' + cpu % 10);
            cpu_line[n] = 0;
        } else if (n + 1 < sizeof cpu_line) {
            cpu_line[n++] = (char)('0' + cpu % 10);
            cpu_line[n] = 0;
        }
    }

    const char *lines[14];
    int n = 0;
    lines[n++] = ":(";
    lines[n++] = "";
    lines[n++] = "AuraLite OS";
    lines[n++] = "A problem has been detected and AuraLite has been shut down.";
    lines[n++] = "";
    lines[n++] = stop_line;
    lines[n++] = mean;
    if (detail && detail[0]) lines[n++] = detail;
    lines[n++] = "";
    lines[n++] = cpu_line;
    lines[n++] = rip_line;
    lines[n++] = extra_line;
    lines[n++] = "";
    lines[n++] = "This STOP code is documented in docs/bsod.md.  System halted.";

    gfx_bsod_seize();
    fb_bsod_paint(lines, n);
}

#endif /* !AURALITE_BSOD_HOST_TEST */
