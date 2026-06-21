#ifndef AURALITE_KERNEL_H
#define AURALITE_KERNEL_H

/* Global, architecture-independent kernel definitions. */

#define AURALITE_NAME    "AuraLite OS"
#define AURALITE_VERSION "0.1.0"

#ifndef ARCH_X86_64
#  error "ARCH_X86_64 must be defined for this build"
#endif

#endif /* AURALITE_KERNEL_H */
