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
    puts("  OS         : AuraLite OS 1.0.0");
    puts("  Arch       : x86_64 (AMD64)");
    puts("  Bootloader : Limine 12.3.3");
    puts("  Features   : SMP, VFS, TCP/IP, DHCP");
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
    puts("  Try: 'ls /', 'cat /hello', 'run /calc'");
    print_bar(44, '=');
    printf("\n");

    return 0;
}
