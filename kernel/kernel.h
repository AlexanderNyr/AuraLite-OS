#ifndef NOVOS_KERNEL_H
#define NOVOS_KERNEL_H

/* Global, architecture-independent kernel definitions. */

#define NOVOS_NAME    "NovOS"
#define NOVOS_VERSION "0.1.0"

#ifndef ARCH_X86_64
#  error "ARCH_X86_64 must be defined for this build"
#endif

#endif /* NOVOS_KERNEL_H */
