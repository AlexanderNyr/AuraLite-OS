/* irqapwake.c -- userspace trigger for the RESIDUE2 T2 / ledger RES-16
 * kernel receipt: a device IRQ waking a hlt-ed AP.
 *
 * The test itself is kernel-side (SYS_IRQ_AP_WAKE): it parks a hlt looper
 * on cpu 1, aims the RTC's I/O APIC redirection at that AP, and counts
 * the deliveries.  This app just invokes it and reports the outcome so an
 * integration case can grep a userspace-visible line too.
 */

#include "unistd.h"
#include "stdio.h"

int main(void) {
    int64_t rc = syscall(SYS_IRQ_AP_WAKE, 0, 0, 0, 0, 0, 0);
    if (rc == 0) {
        printf("IRQAPWAKE PASS: kernel receipt returned 0\n");
        return 0;
    }
    printf("IRQAPWAKE FAIL: syscall returned %lld\n", (long long)rc);
    return 1;
}
