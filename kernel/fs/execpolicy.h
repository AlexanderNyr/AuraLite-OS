#ifndef AURALITE_FS_EXECPOLICY_H
#define AURALITE_FS_EXECPOLICY_H

#include <stdint.h>
#include <stddef.h>

/*
 * Executable installation policy (phase F1 of FSLAYOUT_PLAN.md).
 *
 * A program may only be *created* executable inside a small allowlist of
 * directories.  Everything else in the system stays writable — this is not a
 * read-only filesystem — but a file cannot acquire an execute bit outside the
 * permitted locations, whether at creation time or later through chmod.
 *
 * WHY THIS LIVES IN ITS OWN TRANSLATION UNIT
 *
 * The predicate is pure: no allocation, no kernel state, no locks.  Keeping
 * it separate from vfs.c means the host unit test compiles the shipping code
 * rather than a copy of it, the same arrangement kernel/gpu/gpu_cmdcheck.c
 * uses for the GPU command validator.  A rule that decides whether a program
 * may become executable is worth testing directly, with the inputs a normal
 * run never produces.
 *
 * WHY IN THE KERNEL AND NOT IN THE PACKAGE MANAGER
 *
 * An installer that enforces its own rules constrains only itself.  The check
 * belongs where every path goes through it.
 */

/* The execute bits, in the POSIX permission layout: user, group, other. */
#define EXEC_MODE_BITS  0111u

/*
 * Canonicalise @path into @out.
 *
 * Resolves "." and ".." lexically, collapses repeated slashes and drops any
 * trailing slash.  The result always begins with '/' and never ends with one
 * (except for the root itself, which is "/").  ".." at the root is absorbed,
 * matching how a kernel resolves a path that tries to climb above it.
 *
 * This is a *lexical* canonicalisation: it does not consult the filesystem
 * and therefore does not follow symlinks.  That is stated plainly here
 * because it is the limit of what the predicate can promise — see the note in
 * execpolicy.c.
 *
 * Returns 0 on success, or -1 if @path is NULL, empty, relative, or does not
 * fit in @out_len.
 */
int exec_path_canonical(const char *path, char *out, size_t out_len);

/*
 * Is @path allowed to be (or become) executable?
 *
 * @path is canonicalised first, so "/opt/../etc/evil" is judged as
 * "/etc/evil" and refused.  Returns 1 if allowed, 0 if not.
 *
 * A path that cannot be canonicalised is refused: an input the policy cannot
 * understand is not an input it should approve.
 */
int exec_install_allowed(const char *path);

/* The allowlist, exposed for diagnostics and for the test to enumerate.
 * Returns the directory at @index, or NULL past the end. */
const char *exec_install_dir(int index);

#endif /* AURALITE_FS_EXECPOLICY_H */
