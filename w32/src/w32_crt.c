/* w32/src/w32_crt.c — WIN32_PLAN.md phase W32-6.
 *
 * TLS callbacks, static initialisers, and a setjmp-based __try/__except.
 *
 * The SEH shim rests on a fact worth stating, because it decided the whole
 * design: AuraLite's kernel ALREADY delivers CPU faults to user handlers.
 * kernel/arch/x86_64/isr.c maps #DE to SIGFPE and #PF to SIGSEGV and calls
 * signal_raise_fault(), which installs a handler frame and returns to it.
 * So __try/__except needs no kernel change at all -- it is a sigaction plus
 * a siglongjmp.  Discovering that turned this phase from "modify the fault
 * path" into "use the fault path that exists", which is both less risky and
 * less code.
 *
 * The one subtlety is the signal mask.  A signal is blocked while its own
 * handler runs, and jumping out with plain longjmp would leave it blocked
 * forever -- so the SECOND divide-by-zero in a program would kill it, while
 * the first was caught.  That is a horrible bug to debug and it is entirely
 * avoided by using sigsetjmp/siglongjmp with savemask = 1, which restore the
 * mask on the way out.  AuraLite's libc implements those correctly
 * (lib/libc/src/compat.c saves and restores via sigprocmask), so the shim
 * uses them rather than the plain pair.  The phase gate faults twice in a
 * row precisely to prove this.
 */

#ifndef AURALITE_W32_HOST_TEST
#include <signal.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#endif

#include "w32/w32_crt.h"
#include "w32/kernel32.h"   /* W32A-3: TLS module registry */

/* ------------------------------------------------------------------------
 * Image-bounds checking
 *
 * Everything below follows a pointer that came out of the file being run, so
 * every one of them is checked against the mapped image first.  A PE can
 * name a TLS callback array anywhere it likes; following that blindly is how
 * a loader turns a bad file into arbitrary execution.
 * ------------------------------------------------------------------------ */

static int in_image(size_t image_size, uint64_t rva, size_t len) {
    if (rva > image_size) return 0;
    if (len > image_size - rva) return 0;
    return 1;
}

/* Convert an absolute VA from the image into an RVA, refusing anything that
 * does not actually land inside this image. */
static int va_to_rva(unsigned char *base, size_t image_size,
                     uint64_t va, uint64_t *out_rva) {
    uint64_t b = (uint64_t)(uintptr_t)base;
    if (va < b) return 0;
    uint64_t rva = va - b;
    if (rva >= image_size) return 0;
    *out_rva = rva;
    return 1;
}

/* ------------------------------------------------------------------------
 * TLS callbacks
 * ------------------------------------------------------------------------ */

/* W32A-3: validate one TLS directory and register its template +
 * callbacks with the thread runtime.  Returns the slot (0..15) or
 * negative: -1..-5 are the historical malformed codes (kept: w32run and
 * the hostile-input gate depend on them), -6 is registry-full.  *ncb
 * reports the callback count for the loader's message.  Nothing RUNS
 * here: the exe's callbacks run via w32_tls_attach_main, a DLL's via
 * w32_tls_run_module, both after their module is fully loaded. */
static int crt_tls_register(unsigned char *base, size_t image_size,
                            uint32_t dir_rva, uint32_t dir_size, int *ncb) {
    const w32_tls_directory_t *tls;
    uint64_t raw_start_rva = 0;
    uint64_t raw_size = 0;
    const void *raw_init = 0;
    w32_tls_cb_fn cbs[65];
    int n = 0;
    uint64_t cb_rva;
    int slot;

    *ncb = 0;
    if (dir_rva == 0 || dir_size == 0) return -7;   /* no directory */
    if (!in_image(image_size, dir_rva, sizeof(w32_tls_directory_t)))
        return -1;
    tls = (const w32_tls_directory_t *)(void *)(base + dir_rva);

    /* Template range (may be empty: callbacks without data). */
    if (tls->start_address_of_raw_data && tls->end_address_of_raw_data &&
        tls->end_address_of_raw_data > tls->start_address_of_raw_data) {
        uint64_t end_rva;
        if (!va_to_rva(base, image_size, tls->start_address_of_raw_data,
                       &raw_start_rva))
            return -1;
        if (!va_to_rva(base, image_size, tls->end_address_of_raw_data,
                       &end_rva))
            return -1;
        raw_size = end_rva - raw_start_rva;
        if (!in_image(image_size, raw_start_rva, (size_t)raw_size))
            return -1;
        raw_init = (const void *)(base + raw_start_rva);
    }

    /* Callback array (may be absent: data without callbacks). */
    if (tls->address_of_callbacks) {
        if (!va_to_rva(base, image_size, tls->address_of_callbacks, &cb_rva))
            return -2;
        for (;;) {
            uint64_t fn_va;
            uint64_t fn_rva;
            if (!in_image(image_size, cb_rva + (uint64_t)n * 8, 8)) return -3;
            fn_va = *(const uint64_t *)(const void *)
                        (base + cb_rva + (size_t)n * 8);
            if (fn_va == 0) break;
            if (!va_to_rva(base, image_size, fn_va, &fn_rva)) return -4;
            if (n >= 64) return -5;
            cbs[n++] = (w32_tls_cb_fn)(void *)(base + fn_rva);
        }
    }
    *ncb = n;

    slot = w32_tls_register_module((void *)base, raw_init, raw_size,
                                   tls->size_of_zero_fill, cbs, n);
    if (slot < 0)
        return -6;

    /* The TLS index is a DWORD the loader owns: the image reads it to find
     * its slot.  The exe registers first and always lands on slot 0; a DLL
     * gets the next free one.  An out-of-image index pointer is ignored
     * rather than fatal (historical policy: the image is malformed, but
     * nothing has executed on its say-so yet). */
    if (tls->address_of_index) {
        uint64_t idx_rva;
        if (va_to_rva(base, image_size, tls->address_of_index, &idx_rva) &&
            in_image(image_size, idx_rva, sizeof(uint32_t))) {
            *(uint32_t *)(void *)(base + idx_rva) = (uint32_t)slot;
        }
    }
    return slot;
}

int w32_crt_run_tls_callbacks(unsigned char *base, size_t image_size,
                              uint32_t dir_rva, uint32_t dir_size) {
    /* No TLS directory is the common case and not an error. */
    int ncb = 0;
    int slot;
    if (dir_rva == 0 || dir_size == 0) return 0;
    slot = crt_tls_register(base, image_size, dir_rva, dir_size, &ncb);
    if (slot == -7) return 0;
    if (slot < 0) return slot;
    return ncb;
}

/* Register a DLL's TLS directory at LoadLibrary time.  Returns the slot
 * or negative (same codes as above, -7 for "no directory"). */
int w32_crt_register_tls(unsigned char *base, size_t image_size,
                         uint32_t dir_rva, uint32_t dir_size) {
    int ncb = 0;
    if (dir_rva == 0 || dir_size == 0) return -7;
    return crt_tls_register(base, image_size, dir_rva, dir_size, &ncb);
}

/* ------------------------------------------------------------------------
 * Static initialisers (.CRT$XCA .. .CRT$XCZ)
 *
 * The linker sorts contributions by section name, so the table is bracketed
 * by the empty XCA and XCZ markers and the constructors land in between.
 * Padding means NULL entries appear inside the range; they are skipped, not
 * treated as a terminator -- treating them as the end would silently drop
 * every constructor after the first padded gap.
 * ------------------------------------------------------------------------ */

int w32_crt_run_initializers(unsigned char *base, size_t image_size,
                             uint32_t start_rva, uint32_t end_rva) {
    if (start_rva == 0 || end_rva <= start_rva) return 0;
    if (!in_image(image_size, start_rva, end_rva - start_rva)) return -1;

    int ran = 0;
    for (uint32_t rva = start_rva; rva + 8 <= end_rva; rva += 8) {
        uint64_t fn_va = *(const uint64_t *)(const void *)(base + rva);
        if (fn_va == 0) continue;            /* padding */

        uint64_t fn_rva;
        if (!va_to_rva(base, image_size, fn_va, &fn_rva)) return -2;

        void (W32ABI *ctor)(void) = (void (W32ABI *)(void))(void *)
                                        (base + fn_rva);
        ctor();
        ran++;
    }
    return ran;
}

/* ------------------------------------------------------------------------
 * __try / __except
 * ------------------------------------------------------------------------ */

/* W32A-4: the shim is deleted (see the header note).  The fault entry --
 * w32_seh_init and everything behind it -- lives in w32/src/w32_seh.c,
 * dispatching through the image's own .pdata/.xdata. */
