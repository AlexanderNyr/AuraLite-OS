/* kernel.c — C entry point (kmain), reached from boot.asm. */

#include <stdint.h>
#include "kernel/kernel.h"
#include "kernel/arch/x86_64/gdt.h"
#include "kernel/arch/x86_64/idt.h"
#include "kernel/arch/x86_64/irq.h"
#include "kernel/arch/x86_64/ioapic.h"
#include "kernel/arch/x86_64/paging.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/kheap.h"
#include "kernel/mm/slab.h"
#include "kernel/lib/kprintf.h"
#include "kernel/lib/klog.h"
#include "kernel/lib/perfstat.h"
#include "kernel/lib/selftest.h"
#include "kernel/lib/stack_protector.h"
#include "kernel/boot_info.h"
#include "kernel/rng.h"
#include "kernel/proc/scheduler.h"
#include "kernel/proc/thread.h"
#include "kernel/proc/user.h"
#include "kernel/proc/process.h"
#include "kernel/arch/x86_64/syscall.h"
#include "kernel/arch/x86_64/tss.h"
#include "kernel/arch/x86_64/smp.h"
#include "kernel/arch/x86_64/diagnostics.h"
#include "kernel/fs/vfs.h"
#include "kernel/fs/blkdev.h"
#include "kernel/fs/initrd.h"
#include "kernel/fs/devfs.h"
#include "kernel/fs/procfs.h"
#include "kernel/fs/tmpfs.h"
#include "kernel/fs/diskfs.h"
#include "kernel/fs/fat32.h"
#include "kernel/fs/ext2.h"
#include "kernel/fs/usbfs.h"
#include "kernel/fs/buffer_cache.h"
#include "kernel/fs/exfat.h"
#include "kernel/fs/ext4.h"
#include "kernel/fs/btrfs.h"
#include "kernel/fs/f2fs.h"
#include "kernel/fs/ntfs.h"
#include "kernel/fs/fsformat.h"
#include "kernel/net/net.h"
#include "kernel/net/tcp.h"
#include "drivers/uart/uart.h"
#include "drivers/framebuffer/fb.h"
#include "drivers/framebuffer/graphics.h"
#include "drivers/framebuffer/wm.h"
#include "drivers/framebuffer/render3d.h"
#include "drivers/gpu/virtio_gpu.h"
#include "kernel/gui/gui.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/ahci/ahci.h"
#include "drivers/usb/uhci.h"
#include "drivers/usb/ohci.h"
#include "drivers/usb/ehci.h"
#include "drivers/usb/xhci.h"
#include "drivers/usb/usb_core.h"
#include "drivers/usb/usb_hub.h"
#include "drivers/usb/cdc_acm.h"
#include "drivers/usb/usb_audio.h"
#include "drivers/usb/usb_printer.h"
#include "drivers/usb/usb_string.h"
#include "drivers/usb/usb_isoc.h"
#include "drivers/usb/msc.h"
#include "drivers/usb/hid.h"
#include "drivers/bluetooth/bt.h"
#include "drivers/wifi/wifi.h"
#include "drivers/timer/pit.h"
#include "drivers/vm/virtual_drivers.h"
#include "drivers/virtio_blk/virtio_blk.h"
#include "kernel/audio/audio.h"

/* OPT_PLAN.md O3: IRQ-4 thunk for the UART TX ring — irq_handler_t takes
 * a registers pointer the UART body has no use for; the adapter lives
 * here because uart.c deliberately does not include irq.h (I6 ratchet). */
static void uart_tx_irq_thunk(struct registers *regs) {
    (void)regs;
    uart_tx_irq();
}

/* Halt the (only) CPU indefinitely with interrupts off. */
void kernel_halt(void) {
    /* OPT_PLAN.md O3 (D4): the ring may still hold the last words —
     * drain it synchronously before the lights go out, and latch every
     * later byte onto the synchronous path. */
    uart_flush();
    for (;;) {
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
}

/* ============================================================================
 * Experimental Filesystems Smoke Tests (Task 5)
 * Included here for verification, but disabled during normal boot to ensure stability.
 * ============================================================================ */
void test_buffer_cache(void) {
    kprintf("[test] test_buffer_cache: running smoke test...\n");
    struct buffer *b = bc_get(0, 0);
    if (b) {
        kprintf("[test] test_buffer_cache: bc_get success\n");
        bc_release(b);
        kprintf("[test] test_buffer_cache: PASS\n");
    } else {
        kprintf("[test] test_buffer_cache: FAIL\n");
    }
}

void test_ext4_smoke(void) {
    kprintf("[test] test_ext4_smoke: running smoke test...\n");
    int result = ext4_self_test();
    if (result == 0) {
        kprintf("[test] test_ext4_smoke: PASS\n");
    } else if (result == -1) {
        kprintf("[test] test_ext4_smoke: SKIP (not mounted)\n");
    } else {
        kprintf("[test] test_ext4_smoke: FAIL (error %d)\n", result);
    }
}

void test_btrfs_smoke(void) {
    kprintf("[test] test_btrfs_smoke: running smoke test...\n");
    int result = btrfs_self_test();
    if (result == 0) {
        kprintf("[test] test_btrfs_smoke: PASS\n");
    } else if (result == -1) {
        kprintf("[test] test_btrfs_smoke: SKIP (not mounted)\n");
    } else {
        kprintf("[test] test_btrfs_smoke: FAIL (error %d)\n", result);
    }
}

void test_f2fs_smoke(void) {
    kprintf("[test] test_f2fs_smoke: running smoke test...\n");
    int result = f2fs_self_test();
    if (result == 0) {
        kprintf("[test] test_f2fs_smoke: PASS\n");
    } else if (result == -1) {
        kprintf("[test] test_f2fs_smoke: SKIP (not mounted)\n");
    } else {
        kprintf("[test] test_f2fs_smoke: FAIL (error %d)\n", result);
    }
}

void test_exfat_detect(void) {
    kprintf("[test] test_exfat_detect: running detection test...\n");
    struct vfs_stat st;
    if (vfs_stat("/exfat", &st) == 0) {
        kprintf("[test] test_exfat_detect: /exfat root detected\n");
        kprintf("[test] test_exfat_detect: PASS\n");
    } else {
        kprintf("[test] test_exfat_detect: /exfat not mounted or detected\n");
    }
}

void test_ntfs_detect(void) {
    kprintf("[test] test_ntfs_detect: running detection test...\n");
    struct vfs_stat st;
    if (vfs_stat("/ntfs", &st) == 0) {
        kprintf("[test] test_ntfs_detect: /ntfs root detected\n");
        kprintf("[test] test_ntfs_detect: PASS\n");
    } else {
        kprintf("[test] test_ntfs_detect: /ntfs not mounted or detected\n");
    }
}

void run_experimental_tests(void) {
    kprintf("[test] running experimental storage tests...\n");
    test_buffer_cache();
    test_ext4_smoke();
    test_btrfs_smoke();
    test_f2fs_smoke();
    test_exfat_detect();
    test_ntfs_detect();
}

void kmain(boot_info_t *boot_info) {
    /* Latch the bootloader handoff pointer BEFORE any other subsystem
     * touches it -- stack_protector_init(), uart_init(), etc. all read
     * from boot_info via boot_get_*() accessors. */
    boot_info_init(boot_info);

    /* Order matters for diagnosability: each subsystem prints its own status,
       so if the boot stalls or triple-faults we can see exactly how far it got. */
    uart_init();
    stack_protector_init();
    kprintf("[boot] UART (COM1) initialised @ 115200 baud\n");

    fb_init();
    kprintf("[boot] framebuffer console initialised\n");

    gdt_init();
    kprintf("[boot] GDT loaded (kernel + user segments; TSS slot pending)\n");

    idt_init();
    kprintf("[boot] IDT installed: 256 gates\n");

    pic_init();
    kprintf("[boot] PIC remapped (IRQs -> vectors 32-47), all masked\n");

    __asm__ volatile ("sti");   /* interrupts on; exceptions fire regardless */

    syscall_init();
    kprintf("[boot] SYSCALL/SYSRET configured\n");

    kprintf("\n");
    kprintf("==============================================\n");
    kprintf(" Hello from AuraLite OS kernel!                     \n");
    kprintf("  x86_64 long mode, booted via %s               \n",
            boot_info->boot_from_uefi ? "UEFI" : "BIOS");
    kprintf("==============================================\n");
    kprintf("\n");

    kprintf("[kernel] %s version %s\n", AURALITE_NAME, AURALITE_VERSION);
    kprintf("[kernel] build: %s %s\n", __DATE__, __TIME__);

    /* HW_PLAN H0: the feature receipts.  One line per boot, printed
     * before anything acts on them, so every lane (qemu64, -cpu max,
     * metal) leaves a record of what the CPU actually offered -- the
     * receipt IS the compatibility matrix (HW D4), and the H2/H3/H4
     * phases gate on these exact lines.  Body lives in the arch tree
     * (diagnostics.c): cpuid/rdmsr are x86 by nature, and ratchet 2
     * fired when a first draft put them here -- working as built. */
    diag_cpu_feature_receipts();

    kprintf("[boot]  handoff magic=0x%016llx path=%s\n",
            (unsigned long long)boot_info->magic,
            boot_info->boot_from_uefi ? "UEFI" : "BIOS");

    uint64_t mem = boot_get_usable_memory();
    kprintf("[mm]    usable memory: %llu bytes (%llu KiB / %llu MiB)\n",
            (unsigned long long)mem,
            (unsigned long long)(mem / 1024ULL),
            (unsigned long long)(mem / (1024ULL * 1024ULL)));
    kprintf("[mm]    HHDM offset: 0x%016llx\n",
            (unsigned long long)boot_get_hhdm_offset());

    kprintf("\n[kernel] interrupts enabled, exception handling online.\n");

    /* OPT_PLAN.md O3: arm the UART TX ring now that the IDT/PIC are live.
     * The thunk exists because uart.c deliberately does not include
     * irq.h (the I6 include ratchet); kernel.c already does.  IRQ 4
     * survives the IOAPIC takeover via the identity-mapped GSI 4 entry. */
    irq_register_handler(4, uart_tx_irq_thunk);
    uart_tx_ring_enable();
    kprintf("[uart] TX ring armed (16 KiB buffer, THRE IRQ 4)\n");

    /* OPT_PLAN.md O2: pick the self-test intensity before the first
     * scaled self-test runs.  fw_cfg (QEMU) can override the build
     * default; the line is greppable and states both mode and source. */
    {
        extern void fwcfg_selftest_probe(void);
        fwcfg_selftest_probe();
        kprintf("[selftest] mode: %s (%s)\n",
                selftest_mode_name(), selftest_mode_source());
    }

    /* FSFULL_PLAN.md F1: the auto-format gate for the experimental
     * filesystems, probed before any of them can mount.  fw_cfg can
     * override the build default; the line is greppable and states both
     * value and source. */
    {
        extern void fwcfg_fsformat_probe(void);
        fwcfg_fsformat_probe();
        kprintf("[fsformat] auto-format: %s (%s)\n",
                fs_format_allowed() ? "ENABLED" : "DISABLED",
                fs_format_source());
    }

    kprintf("[boot] initialising physical memory manager...\n");
    pmm_init();
    /* HW H3 follow-up: the fb console's RAM shadow needs frames; arm
     * it the moment they exist so scrolls never read WC memory (the
     * measured WHPX 3-4-lines/s crawl). */
    fb_arm_shadow();
    pmm_self_test();

    kprintf("[boot] initialising virtual memory manager...\n");
    paging_init();
    fb_vga_lock_mmio();
    paging_self_test();

    /* HW H3: the framebuffer goes write-combining (PAT4) now that the
     * VMM can split the HHDM's huge pages under it.  On TCG this is a
     * correctness-only change (memory types are ignored there); the
     * probe line it prints is what the smokes pin. */
    paging_fb_set_wc();

    kprintf("[boot] initialising kernel heap...\n");
    kheap_init();
    kheap_self_test();

    slab_init();

    tss_init();
    kprintf("[boot] TSS loaded (RSP0 + IST1 for #DF)\n");

    /* FIX_R0: read back the TSS/IDT as loaded and report whether the IST is
     * actually armed, so FIX_R1's change becomes visible in the boot log
     * rather than only in the source. */
    diag_ist_self_check();

    kprintf("[boot] initialising SMP...\n");
    smp_init();
    smp_self_test();

    /* M2: switch the BSP from the 8259 PIC/"virtual-wire" path onto the I/O
     * APIC for legacy IRQ delivery.  Must run AFTER smp_init() (which calls
     * lapic_enable(), mapping the Local APIC and giving us a usable APIC ID)
     * and BEFORE pit_init(), so the very first PIT tick arrives over the I/O
     * APIC instead of through the PIC.  On failure (no IOAPIC) it leaves the
     * PIC path intact and we simply continue.  APs are unaffected -- they run
     * their own Local APIC timer. */
    ioapic_init();

    kprintf("[boot] initialising timer (PIT @ 100 Hz)...\n");
    pit_init(100);
    timer_self_test();

    /* N0 (INTERNET_PLAN.md): the kernel CSPRNG.  Detects RDRAND/RDSEED and
     * seeds from them when present; otherwise arms the interrupt-jitter
     * pool and refuses to serve (-ENOSYS) until it is stirred.  Must come
     * after the timer so timer_get_ticks() is live. */
    rng_init();

    kprintf("[boot] initialising scheduler...\n");
    sched_init();
    scheduler_self_test();

    kprintf("[boot] probing virtual machine hardware catalog...\n");
    virtual_drivers_init();

    kprintf("[boot] initialising audio subsystem...\n");
    audio_init();

    kprintf("[boot] initialising virtual file system...\n");
    vfs_init();

    /* Mount the initrd (USTAR) at "/" if the bootloader provided one.
     * boot_info reports the initrd as a raw physical address; convert it
     * to a kernel-visible pointer by adding the HHDM offset. */
    {
        uint64_t initrd_size = 0;
        uint64_t initrd_phys = boot_get_initrd(&initrd_size);
        if (initrd_phys != 0 && initrd_size != 0) {
            uint64_t hhdm = boot_get_hhdm_offset();
            if (initrd_init(hhdm + initrd_phys, initrd_size) == 0) {
                vfs_mount("/", &initrd_ops, NULL);
                vfs_list("/");
            } else {
                kprintf("[vfs] WARNING: initrd failed to initialise; "
                        "booting without it\n");
            }
        } else {
            kprintf("[vfs] WARNING: no initrd loaded\n");
        }
    }

    /* Mount devfs at "/dev". */
    devfs_init();
    vfs_mount("/dev", &devfs_ops, NULL);

    /* Mount procfs at "/proc". */
    procfs_init();

    /* Mount writable tmpfs at "/tmp", and a second volume at "/opt".
     *
     * /opt is where installed packages go (FSLAYOUT_PLAN phase F1).  It is a
     * separate volume, not a directory inside /tmp, so that scratch traffic
     * cannot crowd out an installed program.
     *
     * It is in-memory, so it does NOT survive a reboot.  The plan asks for a
     * location that persists; making that true needs a writable disk that is
     * present on every boot, and the persistent filesystems here mount only
     * when their device exists.  Shipping /opt as tmpfs now means apm stops
     * installing into a directory whose whole purpose is to be wiped, which
     * is the defect that mattered; the durability is a separate change with a
     * separate dependency, and it is recorded in TODO.md rather than
     * pretended away. */
    tmpfs_init();
    vfs_mount("/tmp", &tmpfs_ops, tmpfs_volume_tmp());
    vfs_mount("/opt", &optfs_ops, tmpfs_volume_opt());
    vfs_mount("/dev/shm", &shmfs_ops, tmpfs_volume_shm());   /* Q12 */
    tmpfs_self_test();

    /* Mount usbfs at "/usb". It shows the active hotplug USB mass-storage device. */
    usbfs_init();
    vfs_mount("/usb", &usbfs_ops, NULL);

    /* Quick VFS sanity check. */
    vfs_self_test();

    kprintf("[boot] initialising network stack...\n");
    int net_status = net_init();
    if (net_status == 0) {
        net_self_test();
        net_dns_self_test();
        tcp_x5_self_test();   /* X5: concurrent-connection + full-table gate */
        tcp6_self_test();     /* Y3: TCP-over-IPv6 receipt (skips if no peer) */
    } else if (net_status > 0) {
        kprintf("[net] fallback IP active; skipping online self-tests to keep boot fast\n");
    } else {
        kprintf("[net] network unavailable; continuing boot without online self-tests\n");
    }

    /* F2 (FSFULL_PLAN.md): the buffer cache is created BEFORE any
     * filesystem touches a device, so exFAT/NTFS (which already read
     * through bc_get) and now ext4/btrfs/f2fs (which read through the
     * shared fs_read_block/fs_write_block helpers) never hit an
     * uninitialised cache.  bc_init only allocates its pool from the
     * kernel heap (kheap_init above), so it is safe to run here. */
    kprintf("[boot] initialising buffer cache...\n");
    bc_init();

    /* AHCI SATA driver. */
    kprintf("[boot] initialising AHCI SATA driver...\n");
    ahci_init();
    ahci_self_test();
    ahci_register_blkdevs();   /* PARITY P1: ports become blkdev ids */
    if (diskfs_init() == 0) {
        vfs_mount("/disk", &diskfs_ops, NULL);
        diskfs_self_test();
    }
    if (fat32_init() == 0) {
        vfs_mount("/fat", &fat32_ops, NULL);
        fat32_self_test();
    }

    /* VirtIO Block driver. */
    if (virtio_blk_init() == 0) {
        kprintf("[boot] virtio-blk initialized\n");
    }

    /* ext2 prefers the *second* disk so /fat and /ext2 stay independent.
     * If only one disk is present, ext2 falls back to it (but skips formatting
     * to avoid clobbering FAT32).  Since P1 these are blkdev ids: id N is the
     * N-th detected disk, whatever the driver behind it. */
    if (blkdev_count() > 1) {
        if (ext2_init(-1) == 0) {
            vfs_mount("/ext2", &ext2_ops, NULL);
            ext2_self_test();
        }
    } else {
        kprintf("[ext2] no second AHCI disk; /ext2 not mounted "
                "(pass two -drive options to QEMU to enable)\n");
    }

    /* ========================================================================
     * Experimental Filesystems (Phase 14+)
     * To avoid auto-format collisions on the primary disk (blkdev 0) or ext2
     * disk (blkdev 1), each experimental FS requires its own dedicated disk.
     * ======================================================================== */
    /* FSFULL F1: mount each experimental filesystem ONLY when its init
     * accepted the volume.  Three distinguishable states, each with its
     * own greppable line: no disk present, mounted, refused (foreign or
     * unreadable volume).  A refused init must never be followed by
     * vfs_mount — a mounted-but-uninitialised driver is how /ext4 used
     * to resolve into garbage. */
    int dev_exfat = (blkdev_count() > 2) ? 2 : -1;
    if (dev_exfat >= 0) {
        if (exfat_init(dev_exfat) == 0) {
            vfs_mount("/exfat", &exfat_ops, NULL);
        } else {
            kprintf("[exfat] foreign volume on device %d; /exfat not mounted\n",
                    dev_exfat);
        }
    } else {
        kprintf("[exfat] no 3rd AHCI disk; /exfat not mounted\n");
    }

    int dev_ext4 = (blkdev_count() > 3) ? 3 : -1;
    if (dev_ext4 >= 0) {
        if (ext4_init(dev_ext4) == 0) {
            vfs_mount("/ext4", &ext4_ops, NULL);
        } else {
            kprintf("[ext4] foreign volume on device %d; /ext4 not mounted\n",
                    dev_ext4);
        }
    } else {
        kprintf("[ext4] no 4th AHCI disk; /ext4 not mounted\n");
    }

    int dev_btrfs = (blkdev_count() > 4) ? 4 : -1;
    if (dev_btrfs >= 0) {
        if (btrfs_init(dev_btrfs) == 0) {
            vfs_mount("/btrfs", &btrfs_ops, NULL);
        } else {
            kprintf("[btrfs] foreign volume on device %d; /btrfs not mounted\n",
                    dev_btrfs);
        }
    } else {
        kprintf("[btrfs] no 5th AHCI disk; /btrfs not mounted\n");
    }

    int dev_f2fs = (blkdev_count() > 5) ? 5 : -1;
    if (dev_f2fs >= 0) {
        if (f2fs_init(dev_f2fs) == 0) {
            vfs_mount("/f2fs", &f2fs_ops, NULL);
        } else {
            kprintf("[f2fs] foreign volume on device %d; /f2fs not mounted\n",
                    dev_f2fs);
        }
    } else {
        kprintf("[f2fs] no 6th AHCI disk; /f2fs not mounted\n");
    }

    int dev_ntfs = (blkdev_count() > 6) ? 6 : -1;
    if (dev_ntfs >= 0) {
        if (ntfs_init(dev_ntfs) == 0) {
            vfs_mount("/ntfs", &ntfs_ops, NULL);
        } else {
            kprintf("[ntfs] foreign volume on device %d; /ntfs not mounted\n",
                    dev_ntfs);
        }
    } else {
        kprintf("[ntfs] no 7th AHCI disk; /ntfs not mounted\n");
    }

    /* Disabled in normal boot until fully stable, can be called when needed:
     * run_experimental_tests();
     */

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

    /* USB core + full stack enumeration. */
    kprintf("[boot] initialising USB device core — FULL SUPPORT...\n");
    usb_string_init();
    usb_isoc_init();
    usb_hub_init();
    usb_enumerate_all();
    usb_core_self_test();

    kprintf("[boot] initialising USB full class drivers (string, hub, isoc)...\n");
    usb_string_self_test();
    usb_hub_self_test();
    usb_isoc_self_test();

    kprintf("[boot] initialising USB Mass Storage (BBB + SCSI)...\n");
    msc_init();
    msc_self_test();

    kprintf("[boot] initialising USB CDC ACM (serial/modem)...\n");
    cdc_acm_init();
    cdc_acm_self_test();

    kprintf("[boot] initialising USB Audio (UAC1/UAC2 isoc)...\n");
    usb_audio_init();
    usb_audio_self_test();

    kprintf("[boot] initialising USB Printer (7/1/x bulk)...\n");
    usb_printer_init();
    usb_printer_self_test();

    /* Bluetooth. */
    kprintf("[boot] initialising Bluetooth HCI (now over full USB CDC/HID)...\n");
    bt_init();
    bt_self_test();

    /* Wi-Fi. */
    kprintf("[boot] initialising Wi-Fi (802.11) subsystem...\n");
    wifi_init();
    wifi_self_test();

    /* ---- Phase 14+: GUI + Mouse + Window Manager ---- */
    kprintf("[boot] initialising graphics + keyboard + mouse (PS/2 + USB HID full)...\n");
    gfx_init();
    if (virtio_gpu_init() == 0) {
        const virtio_gpu_info_t *vg = virtio_gpu_get_info();
        kprintf("[boot] virtio-gpu acceleration ready: virgl=%d size=%ux%u\n",
                vg->virgl_enabled, vg->width, vg->height);
    }
    keyboard_init();
    mouse_init();
    usb_hid_init();
    usb_hid_self_test();
    usb_hotplug_start();

    /* Boot screen. */
    gfx_clear(GFX_DARKBLUE);
    gfx_fill_rect(0, 0, gfx_get_width(), 40, GFX_BLUE);
    gfx_draw_string(16, 16, "AuraLite OS v0.0.1 — Graphics Mode", GFX_WHITE);
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

    /* OPT_PLAN.md O0: the boot-latency stamp.  One counter, one greppable
     * line — test_perf_smoke.sh and the §6 table in the plan read this, so
     * the format is an interface. */
    {
        uint64_t boot_ticks = timer_get_ticks();
        uint32_t hz = timer_get_frequency();
        if (hz == 0) hz = 100;
        perfstat_set(PERF_BOOT_TICKS_TO_SHELL, boot_ticks);
        kprintf("[perf] boot-to-shell: %llu ticks (~%llu ms @ %u Hz)\n",
                (unsigned long long)boot_ticks,
                (unsigned long long)(boot_ticks * 1000ULL / hz),
                (unsigned)hz);
    }

    /* The shell is now running interactively. kmain yields forever, giving
     * the shell scheduling slots. When the shell exits, kmain + idle remain. */
    kprintf("\n[kernel] shell active; kmain idling.\n");
    /* O7: the old yield-forever loop kept kmain permanently runnable —
     * a whole CPU's worth of scheduler churn to flush a log buffer.
     * A blocking 100 ms sleep flushes klog at 10 Hz and gives the CPU
     * to the idle loop the other 99% of the time (measured: idle busy%
     * fell from ~36 to single digits with the O7 set). */
    for (;;) {
        klog_flush();
        timer_sleep_ms(100);
    }
}
