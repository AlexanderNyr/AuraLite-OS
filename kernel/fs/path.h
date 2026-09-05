/* kernel/fs/path.h — lexical path canonicalisation (RESIDUE2 T4).
 *
 * A pure, allocation-free lexical canonicaliser shared by the VFS resolver,
 * the installation policy and (through them) anything else that must judge a
 * path.  Keeping it out of vfs.c means the host-side unit test
 * (tests/unit/test_vfs_path.c) links exactly the code the kernel runs —
 * the keymap.c pattern.
 *
 * Semantics (POSIX realpath(3), lexical half):
 *   - repeated '/' collapse to one;
 *   - "." components vanish;
 *   - ".." cancels the previous component, and at the root is absorbed
 *     ("/.." is "/", as on every POSIX system);
 *   - a trailing component that is "." or ".." is resolved lexically
 *     ("/tmp/." -> "/tmp", "/tmp/.." -> "/");
 *   - the result has no trailing '/' except the root itself.
 *
 * What this deliberately does NOT do: follow symlinks.  That is the caller's
 * job (vfs.c walks components and expands links between canonicalisation
 * rounds; execpolicy.c asks the VFS the same question).  Lexical ".."
 * handling and link following compose: expand a link, canonicalise again,
 * keep walking — bounded by VFS_SYMLINK_MAX_FOLLOW.
 */

#ifndef AURALITE_KERNEL_FS_PATH_H
#define AURALITE_KERNEL_FS_PATH_H

#include <stddef.h>

/*
 * Canonicalise @in into @out (buffer of @out_len bytes).
 *
 * @in   must be absolute (leading '/'); relative paths are refused.  The VFS
 *       only ever resolves absolute paths (the shell's cwd expansion and the
 *       libc PATH search both absolutise before entering the kernel).
 * Returns 0 on success, -1 if @in is NULL/not absolute/@out too small
 *       (ENOENT/EINVAL/ENAMETOOLONG are the caller's to map — this helper
 *       stays errno-free so both kernel and host tests can link it).
 *
 * @out always receives a NUL-terminated string, even on failure ("" on
 *       refusal), so a caller can never echo an uninitialised buffer.
 */
int vfs_canonical_path(const char *in, char *out, size_t out_len);

#endif /* AURALITE_KERNEL_FS_PATH_H */
