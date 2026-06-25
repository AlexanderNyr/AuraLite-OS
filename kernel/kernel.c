/* kernel.c — C entry point (kmain), reached from boot.asm. */

#include <stdint.h>
#include "kernel/kernel.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/klog.h"
#include "kernel/limine_requests.h"
#include "limine/limine.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/user.h"
#include "kernel/proc/process.h"
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/initrd.h"
#include "kernel/fs/devfs.h"
#include "kernel/fs/procfs.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/diskfs.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/exfat.h"
#include "kernel/fs/ntfs.h"
#include "kernel/fs/usbfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/net/net.h"
#include "kernel/net/tcp.h"
#include "drivers/uart/uart.h"
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/wm.h"
#include "drivers/framebuffer/render3d.h"
#include "kernel/gui/gui.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/ahci/ahci.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/xhci.h"
#include "drivers/usb/usb_core.h"
#include "drivers/usb/msc.h"
#include "drivers/usb/hid.h"
#include "drivers/bluetooth/bt.h"
#include "drivers/wifi/wifi.h"
#include "drivers/timer/pit.h"
#include "drivers/vm/virtual_drivers.h"
#include "kernel/audio/audio.h"

void kernel_halt(void) {
    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void kmain(void) {
    uart_init();
    fb_init();
    gdt_init();
    idt_init();
    pic_init();
    __asm__ volatile ("sti");
    tss_init();
    syscall_init();

    kprintf("\n==============================================\n");
    kprintf(" AuraLite OS - Advanced Storage Edition        \n");
    kprintf("==============================================\n\n");

    pmm_init();
    paging_init();
    kheap_init();
    smp_init();
    pit_init(100);
    sched_init();
    virtual_drivers_init();
    audio_init();

    vfs_init();
    bc_init(); // Buffer Cache is key for modern FS

    /* Mount standard VFS */
    {
        uint64_t mod_count = 0;
        struct limine_file *mod = limine_get_modules(&mod_count);
        if (mod && mod_count >= 1) {
            initrd_init((uint64_t)(uintptr_t)mod->address, mod->size);
            vfs_mount("/", &initrd_ops, NULL);
        }
    }
    devfs_init();
    vfs_mount("/dev", &devfs_ops, NULL);
    procfs_init();
    tmpfs_init();
    vfs_mount("/tmp", &tmpfs_ops, NULL);

    /* Storage Subsystem */
    ahci_init();
    int port = ahci_get_first_port();
    if (port >= 0) {
        // exFAT
        exfat_init(port);
        vfs_mount("/exfat", &exfat_ops, NULL);
        
        // ext4/ext2
        ext2_init(port);
        vfs_mount("/ext4", &ext2_ops, NULL);
        
        // NTFS
        ntfs_init(port);
        vfs_mount("/ntfs", &ntfs_ops, NULL);
    }

    /* USB and others */
    uhci_init(); ohci_init(); ehci_init(); xhci_init();
    usb_enumerate_all();
    msc_init();
    bt_init();
    wifi_init();
    gfx_init();
    keyboard_init();
    mouse_init();
    usb_hid_init();
    usb_hotplug_start();

    gui_init();
    extern void gui_compositor_thread(void *arg);
    kthread_create(gui_compositor_thread, NULL, "gui-compositor");

    process_self_test();
    user_mode_self_test();

    kprintf("\n[kernel] Advanced FS support online. Shell active.\n");
    for (;;) {
        klog_flush();
        sched_yield();
    }
}
