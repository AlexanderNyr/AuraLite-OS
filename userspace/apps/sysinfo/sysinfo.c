/*
 * sysinfo.c — system information display for AuraLite OS.
 *
 * Shows OS version, architecture, uptime, PID, and a decorative banner.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static void print_bar(int len, char c) {
    for (int i = 0; i < len; i++) putchar(c);
    putchar('\n');
}

int main(void) {
    printf("\n");
    print_bar(44, '=');
    printf("         AuraLite OS System Information\n");
    print_bar(44, '=');

    pid_t pid = getpid();
    printf("  Process ID : %d\n", (int)pid);
    puts("  OS         : AuraLite OS 0.0.1");
    puts("  Arch       : x86_64 (AMD64)");
    puts("  Bootloader : Custom BIOS/UEFI (BL1-BL8)");
    puts("  Features   : SMP, VFS, TCP/IP, DHCP, TLS");
    /* REALINTERNET_PLAN X9: these numbers must match docs/tls.md and the
     * actual kernel constants (kernel/proc/process.c SPAWN_MAX_IMAGE and
     * USER_STACK_SIZE).  They document the fit of the shipped browser stack. */
    puts("  Exec limit : 1 MiB (SPAWN_MAX_IMAGE; gbrowser uses ~36%)");
    puts("  User stack : 1 MiB (USER_STACK_SIZE)");
    puts("  Shell      : Interactive (serial I/O)");

    print_bar(44, '-');
    puts("  Kernel subsystems:");
    puts("    [x] GDT + IDT (256 gates)");
    puts("    [x] PMM (bitmap, 4 KiB frames)");
    puts("    [x] VMM (4-level paging, NX)");
    puts("    [x] Heap (first-fit, coalescing)");
    puts("    [x] Scheduler (preemptive round-robin)");
    puts("    [x] Ring 3 (SYSCALL/SYSRET)");
    puts("    [x] ELF loader");
    puts("    [x] VFS + USTAR initrd");
    puts("    [x] DevFS (/dev/null, /dev/zero)");
    puts("    [x] Network (ARP/IP/ICMP/UDP/TCP/DHCP/DNS)");
    puts("    [x] SMP (multi-core)");
    puts("    [x] Graphics (double-buffered 2D + WM)");
    puts("    [x] Keyboard + Mouse");
    puts("    [x] Per-process address spaces");

    print_bar(44, '-');
    puts("  Try: 'ls /', 'ls /apps', 'run calc'");
    print_bar(44, '=');
    printf("\n");

    return 0;
}
