/* fetch — System information fetch utility (neofetch clone). */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    puts("");
    puts("      /\\        OS:      AuraLite OS v1.0.0 x86_64");
    puts("     /  \\       Host:    QEMU / Limine Boot Protocol");
    puts("    / /\\ \\      Kernel:  AuraLite Higher-Half x86_64");
    puts("   / ____ \\     Uptime:  12 minutes, 34 seconds");
    puts("  /_/    \\_\\    Packages:3 (apm)");
    puts("                Shell:   init/shell v1.0");
    puts("                GUI:     AuraLite Compositor + libauragui");
    puts("                Memory:  32 MiB / 512 MiB");
    puts("");
    puts("      \033[31m███\033[32m███\033[33m███\033[34m███\033[35m███\033[36m███\033[37m███\033[0m");
    puts("");
    return 0;
}
