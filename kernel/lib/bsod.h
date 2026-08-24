#ifndef AURALITE_LIB_BSOD_H
#define AURALITE_LIB_BSOD_H

#include <stdint.h>

/*
 * Fatal stop codes — the numbers painted on the blue screen and
 * printed as `[bsod] STOP=0x........` on the serial console.
 *
 * Layout (see docs/bsod.md):
 *
 *   0x00000000..0x0000001F   CPU exception vector (Intel SDM Vol.3)
 *   0x00001001..0x000010FF   software / kernel-initiated stops
 *
 * User-mode faults are NOT stop codes: they become POSIX signals.
 * Only a kernel-mode fatal path paints the screen and halts.
 */

#define BSOD_STOP_CPU(vec)     ((uint32_t)((vec) & 0x1Fu))

#define BSOD_KASSERT           0x00001001u  /* ASSERT() failed          */
#define BSOD_KEXPLICIT         0x00001002u  /* explicit kernel stop     */
#define BSOD_KCANARY           0x00001003u  /* stack-protector trip     */
#define BSOD_KSTACK            0x00001004u  /* kernel stack guard hit   */
#define BSOD_KRECURSE          0x00001005u  /* fault inside the dump    */
#define BSOD_KHALT             0x000010FFu  /* halt with no other code  */

/* Short symbolic name, never NULL (unknown -> "UNKNOWN"). */
const char *bsod_stop_name(uint32_t stop);

/* One-line meaning for the screen and the docs, never NULL. */
const char *bsod_stop_meaning(uint32_t stop);

/*
 * Paint the blue screen and emit the serial `[bsod]` banner.
 * Safe to call more than once: the second call is a no-op so a
 * cascading fault cannot flicker the display.
 *
 *   stop    the STOP code
 *   detail  optional extra sentence (may be NULL)
 *   cpu     diag_cpu_id() — passed in so this file stays portable
 *   rip     faulting RIP, or 0 if unknown
 *   extra   CR2 for #PF, error code otherwise, or 0
 */
void bsod_show(uint32_t stop, const char *detail,
               uint32_t cpu, uint64_t rip, uint64_t extra);

#endif /* AURALITE_LIB_BSOD_H */
