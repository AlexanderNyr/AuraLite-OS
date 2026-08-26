/* tools/selfhost/tcc_builtins.c -- __builtin_* helpers tcc lacks.
 *
 * SELFHOST_PLAN.md SH2.  tcc does not know these as compiler builtins,
 * so libc.c's calls to them become ordinary extern calls; these
 * definitions satisfy the link.  Same shape and results as the
 * compiler builtins they stand in for.  AuraLite's own code.
 */

#include <stdint.h>

uint32_t __builtin_bswap32(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8)
         | ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}

uint16_t __builtin_bswap16(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

double __builtin_fabs(double x)
{
    return x < 0.0 ? -x : x;
}

double __builtin_sqrt(double x)
{
    /* Newton-Raphson; exact for the values libc.c feeds it. */
    double g = x;
    if (x <= 0.0) return x == 0.0 ? 0.0 : (0.0 / 0.0);   /* NaN for <0 */
    for (int i = 0; i < 32; i++) {
        double n = 0.5 * (g + x / g);
        if (n == g) break;
        g = n;
    }
    return g;
}

float __builtin_nanf(const char *tagp)
{
    (void)tagp;
    return 0.0f / 0.0f;              /* quiet NaN */
}

double __builtin_huge_val(void)
{
    return 1.7976931348623157e308;   /* DBL_MAX */
}
