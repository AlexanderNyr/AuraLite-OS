/* w32/include/w32/w32_argv.h — WIN32_PLAN.md phase W32-6.
 *
 * Win32 command-line -> argc/argv splitting.
 *
 * Windows hands a process one string and each program splits it itself, so
 * the splitting rules are observable platform behaviour rather than an
 * implementation detail.  w32_argv.c states the rules; this header is the
 * interface.
 *
 * Declarations here are AuraLite's own: this is not a Win32 API surface, it
 * is the machinery behind one, so nothing in it is derived from anyone's
 * headers.
 */
#ifndef AURALITE_W32_ARGV_H
#define AURALITE_W32_ARGV_H

#include <stddef.h>

#define W32_ARGV_OK        0
#define W32_ARGV_EINVAL   -1   /* NULL cmdline or NULL out-parameter        */
#define W32_ARGV_ENOSPACE -2   /* buf too small; *out_bytes says how much   */
#define W32_ARGV_ETOOMANY -3   /* more arguments than max_argv slots        */

/* Split @cmdline into arguments.
 *
 * Two-pass by design, in the same style as w32_utf.h: pass argv == NULL and
 * buf == NULL to MEASURE (*out_argc and *out_bytes are filled, nothing is
 * written), then call again with buffers of that size.  Both passes run the
 * identical parsing code, so the measured size and the written size cannot
 * disagree.
 *
 * @argv receives pointers INTO @buf, so @buf must outlive @argv.  The vector
 * is not NULL-terminated -- *out_argc is the count.
 *
 * Returns W32_ARGV_OK, or a negative W32_ARGV_* code.  On ENOSPACE/ETOOMANY
 * the out-parameters still report the true sizes so the caller can retry.
 */
int w32_cmdline_to_argv(const char *cmdline,
                        char **argv, size_t max_argv,
                        char *buf, size_t buf_cap,
                        size_t *out_argc, size_t *out_bytes);

#endif /* AURALITE_W32_ARGV_H */
