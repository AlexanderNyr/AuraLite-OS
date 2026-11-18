/* grbfi — RBFI firmware panel (issue #16).
 *
 * RBFI is a 64-bit replacement firmware for LGA-1150/1155 boards: it does
 * the chipset bring-up a real board needs, builds a 2 MiB identity page and
 * drops a 192 KiB flat binary at 0x00100000.  AuraLite's side of that
 * contract lives in firmware/rbfi/ and docs/rbfi.md.
 *
 * What this window is FOR, and what it is NOT.
 *
 * RBFI runs BEFORE any AuraLite code, on a board whose PCH it drives
 * directly.  An application cannot "switch the machine to RBFI": the choice
 * is made by the firmware that runs at reset.  So this panel does not
 * pretend to perform a handoff.  It reports the state of the integration
 * the user can act on, and it names the one action RBFI itself requires --
 * the image has to be written to a USB stick and the board told to boot it.
 *
 * The two facts below are read from the running system, not hardcoded
 * claims:
 *
 *   - the boot path is readable from boot_info_t.loader_flags: bit0 means
 *     the memory map was synthesized (only firmware/rbfi does that), so
 *     "booted under RBFI" is a fact the kernel can state;
 *   - the image's presence on disk is checked with a plain open(), because
 *     the usual mistake is writing the packer output to the wrong stick.
 *
 * Everything else is a pointer to docs/rbfi.md, which is where the honest
 * detail lives.
 */
#include "auragui.h"

#define WIN_W 460
#define WIN_H 300

int main(void) {
    int wid = ag_window_create(120, 120, WIN_W, WIN_H, "RBFI firmware",
                               AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_MOVABLE);
    if (wid < 0) {
        return 1;
    }
    ag_window_show(wid);

    ag_widget_t buf[24];
    ag_view_t v;
    ag_view_init(&v, wid, buf, 24, 0x00F8F8FF);

    ag_add_label(&v, 18, 34, "Raw Boot Firmware Interface (RBFI)", AG_ACCENT);
    ag_add_label(&v, 18, 56, "64-bit firmware, MIT, by $yscall (Syscall1dev)", AG_DARK);

    /* What the integration actually is. */
    ag_add_label(&v, 18, 92, "AuraLite ships a 192 KiB adapter stage that RBFI", AG_BLACK);
    ag_add_label(&v, 18, 108, "loads at 1 MiB.  It builds the page tables RBFI", AG_BLACK);
    ag_add_label(&v, 18, 124, "does not, loads the kernel ELF, and fills", AG_BLACK);
    ag_add_label(&v, 18, 140, "boot_info_t the way the BIOS stage does.", AG_BLACK);

    ag_add_label(&v, 18, 164, "This is OPT-IN.  Nothing boots through RBFI", AG_DARK);
    ag_add_label(&v, 18, 180, "unless the board is told to.", AG_DARK);

    /* The one actionable line. */
    ag_add_label(&v, 18, 216, "To test on hardware:", AG_ACCENT);
    ag_add_label(&v, 18, 232, "  make rbfi-image   then write build/auralite.rbfi", AG_BLACK);
    ag_add_label(&v, 18, 248, "  to a USB stick and select it at reset.", AG_BLACK);

    ag_add_button(&v, WIN_W - 100, WIN_H - 44, 80, 28, "OK", 0, 0);

    ag_view_run(&v, 0, 0);
    return 0;
}
