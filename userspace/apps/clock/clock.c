/*
 * clock.c — clock/uptime display for AuraLite OS.
 *
 * Reads a timestamp via syscall (if available) or uses a simple
 * counting loop to approximate seconds.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(void) {
    puts("AuraLite Clock");
    puts("");
    puts("UTC time: not yet available (no RTC driver)");
    puts("");
    puts("This system has been running since boot.");
    puts("Use 'uname' for OS version info.");

    /* Simple countdown timer demo. */
    puts("");
    puts("Starting 5-second countdown...");
    for (int i = 5; i > 0; i--) {
        printf("  %d...\n", i);
        /* Busy-wait approximation (timer_sleep_ms is a kernel function,
         * not yet exposed as a syscall). */
        for (volatile int j = 0; j < 5000000; j++) {
            __asm__ volatile ("nop");
        }
    }
    puts("  Done!");
    return 0;
}
