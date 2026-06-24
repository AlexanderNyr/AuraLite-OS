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
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/diskfs.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/ext2.h"
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
#include "drivers/bluetooth/bt.h"
#include "drivers/wifi/wifi.h"
#include "drivers/timer/pit.h"
#include "drivers/vm/virtual_drivers.h"

/* Halt the (only) CPU indefinitely with interrupts off. */
void kernel_halt(void) {
    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

void kmain(void) {
    /* Order matters for diagnosability: each subsystem prints its own status,
       so if the boot stalls or triple-faults we can see exactly how far it got. */
    uart_init();
    kprintf("[boot] UART (COM1) initialised @ 115200 baud\n");

    fb_init();
    kprintf("[boot] framebuffer console initialised\n");

    gdt_init();
    kprintf("[boot] GDT loaded (kernel + user segments + TSS)\n");

    idt_init();
    kprintf("[boot] IDT installed: 256 gates\n");

    pic_init();
    kprintf("[boot] PIC remapped (IRQs -> vectors 32-47), all masked\n");

    __asm__ volatile ("sti");   /* interrupts on; exceptions fire regardless */

    tss_init();
    kprintf("[boot] TSS loaded (RSP0 + IST1 for #DF)\n");

    syscall_init();
    kprintf("[boot] SYSCALL/SYSRET configured\n");

    kprintf("\n");
    kprintf("==============================================\n");
    kprintf(" Hello from AuraLite OS kernel!                     \n");
    kprintf("  x86_64 long mode, booted via Limine         \n");
    kprintf("==============================================\n");
    kprintf("\n");

    kprintf("[kernel] %s version %s\n", AURALITE_NAME, AURALITE_VERSION);
    kprintf("[kernel] build: %s %s\n", __DATE__, __TIME__);

    if (limine_base_revision_supported()) {
        kprintf("[limine] requested base revision supported\n");
    } else {
        kprintf("[limine] WARNING: requested base revision NOT supported\n");
    }

    uint64_t mem = limine_get_usable_memory();
    kprintf("[mm]    usable memory: %llu bytes (%llu KiB / %llu MiB)\n",
            (unsigned long long)mem,
            (unsigned long long)(mem / 1024ULL),
            (unsigned long long)(mem / (1024ULL * 1024ULL)));
    kprintf("[mm]    HHDM offset: 0x%016llx\n",
            (unsigned long long)limine_get_hhdm_offset());

    kprintf("\n[kernel] interrupts enabled, exception handling online.\n");

    kprintf("[boot] initialising physical memory manager...\n");
    pmm_init();
    pmm_self_test();

    kprintf("[boot] initialising virtual memory manager...\n");
    paging_init();
    paging_self_test();

    kprintf("[boot] initialising kernel heap...\n");
    kheap_init();
    kheap_self_test();

    kprintf("[boot] initialising SMP...\n");
    smp_init();
    smp_self_test();

    kprintf("[boot] initialising timer (PIT @ 100 Hz)...\n");
    pit_init(100);
    timer_self_test();

    kprintf("[boot] initialising scheduler...\n");
    sched_init();
    scheduler_self_test();

    kprintf("[boot] probing virtual machine hardware catalog...\n");
    virtual_drivers_init();

    kprintf("[boot] initialising virtual file system...\n");
    vfs_init();

    /* Mount the initrd (USTAR) at "/" if Limine provided a module. */
    {
        uint64_t mod_count = 0;
        struct limine_file *mod = limine_get_modules(&mod_count);
        if (mod != NULL && mod_count >= 1) {
            initrd_init((uint64_t)(uintptr_t)mod->address, mod->size);
            vfs_mount("/", &initrd_ops, NULL);
            vfs_list("/");
        } else {
            kprintf("[vfs] WARNING: no initrd module loaded\n");
        }
    }

    /* Mount devfs at "/dev". */
    devfs_init();
    vfs_mount("/dev", &devfs_ops, NULL);

    /* Mount writable tmpfs at "/tmp". */
    tmpfs_init();
    vfs_mount("/tmp", &tmpfs_ops, NULL);
    tmpfs_self_test();

    /* Quick VFS sanity check. */
    vfs_self_test();

    kprintf("[boot] initialising network stack...\n");
    int net_status = net_init();
    if (net_status == 0) {
        net_self_test();
        net_dns_self_test();
        tcp_self_test();
    } else if (net_status > 0) {
        kprintf("[net] fallback IP active; skipping online self-tests to keep boot fast\n");
    } else {
        kprintf("[net] network unavailable; continuing boot without online self-tests\n");
    }

    /* AHCI SATA driver. */
    kprintf("[boot] initialising AHCI SATA driver...\n");
    ahci_init();
    ahci_self_test();
    if (diskfs_init() == 0) {
        vfs_mount("/disk", &diskfs_ops, NULL);
        diskfs_self_test();
    }
    if (fat32_init() == 0) {
        vfs_mount("/fat", &fat32_ops, NULL);
        fat32_self_test();
    }

    /* ext2 prefers the *second* AHCI disk so /fat and /ext2 stay independent.
     * If only one disk is present, ext2 falls back to it (but skips formatting
     * to avoid clobbering FAT32). */
    if (ahci_get_nth_port(1) >= 0) {
        if (ext2_init(-1) == 0) {
            vfs_mount("/ext2", &ext2_ops, NULL);
            ext2_self_test();
        }
    } else {
        kprintf("[ext2] no second AHCI disk; /ext2 not mounted "
                "(pass two -drive options to QEMU to enable)\n");
    }

    /* USB UHCI + OHCI + EHCI drivers. */
    kprintf("[boot] initialising USB (UHCI) driver...\n");
    uhci_init();
    uhci_self_test();

    kprintf("[boot] initialising USB (OHCI) driver...\n");
    ohci_init();
    ohci_self_test();

    kprintf("[boot] initialising USB (EHCI) driver...\n");
    ehci_init();
    ehci_self_test();

    kprintf("[boot] initialising USB (xHCI) driver...\n");
    xhci_init();
    xhci_self_test();

    /* USB core + Mass Storage. */
    kprintf("[boot] initialising USB device core...\n");
    usb_enumerate_all();
    usb_core_self_test();

    kprintf("[boot] initialising USB Mass Storage...\n");
    msc_init();
    msc_self_test();

    /* Bluetooth. */
    kprintf("[boot] initialising Bluetooth HCI...\n");
    bt_init();
    bt_self_test();

    /* Wi-Fi. */
    kprintf("[boot] initialising Wi-Fi (802.11) subsystem...\n");
    wifi_init();
    wifi_self_test();

    /* ---- Phase 14+: GUI + Mouse + Window Manager ---- */
    kprintf("[boot] initialising graphics + keyboard + mouse...\n");
    gfx_init();
    keyboard_init();
    mouse_init();

    /* Boot screen. */
    gfx_clear(GFX_DARKBLUE);
    gfx_fill_rect(0, 0, gfx_get_width(), 40, GFX_BLUE);
    gfx_draw_string(16, 16, "AuraLite OS v1.0.0 — Graphics Mode", GFX_WHITE);
    uint32_t box_w = 80, box_h = 60, gap = 16;
    color_t colours[] = {GFX_RED, GFX_GREEN, GFX_YELLOW, GFX_CYAN, GFX_MAGENTA};
    for (int i = 0; i < 5; i++) {
        gfx_fill_rect(16 + i * (box_w + gap), 80, box_w, box_h, colours[i]);
    }
    gfx_draw_line(16, 180, 400, 320, GFX_WHITE);
    gfx_draw_string(16, 360, "GUI + Mouse + Window Manager active", GFX_GREEN);
    gfx_flip();

    /* Legacy WM banner — kept so older tests still see the marker line. */
    wm_demo();
    kprintf("[gfx] framebuffer GUI + window manager rendered\n");

    /* 3D renderer demo. */
    kprintf("[3d] rendering 3D demo...\n");
    r3d_demo(30);
    kprintf("[3d] demo complete\n");

    /* New full GUI: init subsystem, run self-test, then kick off compositor
     * thread that pumps mouse/keyboard events into windows and re-renders
     * the screen at ~30 Hz. */
    kprintf("[boot] initialising GUI subsystem...\n");
    gui_init();
    gui_self_test();
    extern void gui_compositor_thread(void *arg);
    kthread_create(gui_compositor_thread, NULL, "gui-compositor");

    /* Phase 15: per-process address spaces — self-test. */
    kprintf("[boot] testing per-process address spaces...\n");
    process_self_test();

    kprintf("[boot] starting init shell (Ring 3)...\n");
    user_mode_self_test();

    /* The shell is now running interactively. kmain yields forever, giving
     * the shell scheduling slots. When the shell exits, kmain + idle remain. */
    kprintf("\n[kernel] shell active; kmain idling.\n");
    for (;;) {
        klog_flush();
        sched_yield();
    }
}
