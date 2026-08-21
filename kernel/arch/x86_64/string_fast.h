/* kernel/arch/x86_64/string_fast.h -- the rep-string backend's one
 * configuration hook (HW_PLAN H2). */

#ifndef AURALITE_ARCH_X86_64_STRING_FAST_H
#define AURALITE_ARCH_X86_64_STRING_FAST_H

/* Read CPUID.7.0:EBX.9 (ERMS) once and set the small-copy crossover:
 * 0 on ERMS parts (fast-string covers small counts), 64 otherwise
 * (the O1-measured default).  Prints the threshold line the smokes
 * pin.  Call once at boot, after the console is up; before it runs,
 * the conservative 64 applies. */
void string_fast_init(void);

#endif /* AURALITE_ARCH_X86_64_STRING_FAST_H */
