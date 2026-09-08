#ifndef AURALITE_KERNEL_H
#define AURALITE_KERNEL_H

/* Global, architecture-independent kernel definitions. */

#define AURALITE_NAME    "AuraLite OS"

/* OTA_PLAN O2: the version macro lives in version.h (see its header
 * comment for why it is a separate, arch-free file). */
#include "kernel/version.h"

#ifndef ARCH_X86_64
#  error "ARCH_X86_64 must be defined for this build"
#endif

#endif /* AURALITE_KERNEL_H */
