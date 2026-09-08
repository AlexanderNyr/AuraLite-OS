#ifndef AURALITE_KERNEL_H
#define AURALITE_KERNEL_H

/* Global, architecture-independent kernel definitions. */

#define AURALITE_NAME    "AuraLite OS"

/* OTA_PLAN O2: the version identity is a BUILD knob, not a source literal.
 * `make iso AURALITE_VERSION=0.0.2-ota` passes -DAURALITE_VERSION='"..."'
 * to every kernel and userspace compile (the shell banner and `uname`
 * read the same macro); this #ifndef keeps non-Makefile compiles and
 * readers honest with the stock value. */
#ifndef AURALITE_VERSION
#define AURALITE_VERSION "0.0.1"
#endif

#ifndef ARCH_X86_64
#  error "ARCH_X86_64 must be defined for this build"
#endif

#endif /* AURALITE_KERNEL_H */
