/* gabout — About / splash box. */
#include "auragui.h"

int main(void) {
    int wid = ag_window_create(280, 200, 360, 220, "About AuraLite OS",
                               AG_WIN_HAS_TITLE | AG_WIN_HAS_CLOSE | AG_WIN_MOVABLE);
    if (wid < 0) return 1;
    ag_window_show(wid);

    ag_widget_t buf[10];
    ag_view_t v;
    ag_view_init(&v, wid, buf, 10, 0x00F8F8FF);

    ag_add_label(&v, 32, 20, "AuraLite OS v1.0", AG_ACCENT);
    ag_add_label(&v, 32, 44, "x86_64 hobby operating system", AG_BLACK);
    ag_add_label(&v, 32, 64, "Booted via Limine, ring 3 userspace", AG_DARK);
    ag_add_label(&v, 32, 88, "Filesystems: initrd / devfs / tmpfs", AG_DARK);
    ag_add_label(&v, 32, 102, "             diskfs / fat32 / ext2", AG_DARK);
    ag_add_label(&v, 32, 126, "Networking: e1000 + ARP + DHCP + TCP", AG_DARK);
    ag_add_label(&v, 32, 146, "USB: UHCI/OHCI/EHCI/xHCI + hotplug", AG_DARK);
    ag_add_label(&v, 32, 160, "GUI: kernel WM + libauragui toolkit", AG_DARK);

    ag_add_button(&v, 140, 180, 80, 28, "OK", 0, 0);
    ag_view_run(&v, 0, 0);
    return 0;
}
