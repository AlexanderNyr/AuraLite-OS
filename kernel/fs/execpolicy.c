/* execpolicy.c — where a program is allowed to be installed.
 *
 * Phase F1 of FSLAYOUT_PLAN.md.  See execpolicy.h for the rationale.
 */

#include "kernel/fs/execpolicy.h"
#include "kernel/lib/string.h"

/*
 * The allowlist.
 *
 *   /opt   installed packages.  This is where `apm` writes.  Before F1 it
 *          wrote to /tmp — the one directory guaranteed to be wiped — so the
 *          package manager worked and installed into scratch space.
 *   /tmp   scratch.  Kept deliberately: the shell, the test programs and the
 *          integration suite all build and run things there, and removing it
 *          would turn a policy change into a functional regression.  /tmp is
 *          tmpfs, so nothing written there survives a reboot; an executable
 *          in /tmp is a temporary executable, which is what it claims to be.
 *
 * The initrd mounts read-only, so /bin, /apps, /demos and /tests — the
 * directories phase F3 introduces — need no entry here.  Nothing can write to
 * them at all, and an allowlist entry for a location that cannot be written
 * would be misleading about where the guarantee comes from.
 *
 * A directory in this list permits its subdirectories too: /opt/foo/bar is
 * inside /opt.
 */
static const char *const allowed_dirs[] = {
    "/opt",
    "/tmp",
};

#define ALLOWED_COUNT ((int)(sizeof(allowed_dirs) / sizeof(allowed_dirs[0])))

const char *exec_install_dir(int index) {
    if (index < 0 || index >= ALLOWED_COUNT) return 0;
    return allowed_dirs[index];
}

int exec_path_canonical(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len < 2) return -1;
    if (path[0] != '/') return -1;      /* only absolute paths are judged */

    size_t w = 0;
    out[w++] = '/';

    const char *p = path;
    while (*p) {
        /* Skip the separator(s) before this component. */
        while (*p == '/') p++;
        if (!*p) break;

        /* Measure the component. */
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.') {
            continue;                    /* "." is a no-op */
        }
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* Climb one level.  At the root this is absorbed, which is what
             * a kernel does: "/.." is "/". */
            while (w > 1 && out[w - 1] != '/') w--;
            if (w > 1) w--;              /* drop the separator too */
            if (w == 0) w = 1;
            continue;
        }

        /* Append "/component", except at the root where the '/' is present. */
        if (w > 1) {
            if (w + 1 >= out_len) return -1;
            out[w++] = '/';
        }
        if (w + clen >= out_len) return -1;
        memcpy(out + w, start, clen);
        w += clen;
    }

    out[w] = '\0';
    return 0;
}

/*
 * A NOTE ON WHAT THIS CANNOT PROMISE
 *
 * exec_path_canonical() is lexical, so it defeats "/opt/../etc/evil" — the
 * obvious way an allowlist is bypassed — but it does not follow symlinks.  A
 * symlink at /tmp/link pointing to /etc would let a write through /tmp/link/x
 * land outside the allowlist.
 *
 * That gap is stated rather than papered over.  Closing it means resolving
 * the parent through the VFS, following links, before judging the path; the
 * VFS's symlink registry (kernel/fs/symlink.c) is in-memory and separate from
 * the mount table, so doing it correctly is more than a line of code.  The
 * policy is honest about its scope: it stops a program from writing an
 * executable to a path it names, not from being led somewhere by a link
 * someone else planted.
 */
int exec_install_allowed(const char *path) {
    char canon[256];
    if (exec_path_canonical(path, canon, sizeof(canon)) != 0) {
        return 0;   /* cannot be understood, so cannot be approved */
    }

    for (int i = 0; i < ALLOWED_COUNT; i++) {
        const char *dir = allowed_dirs[i];
        size_t dlen = strlen(dir);
        if (strncmp(canon, dir, dlen) != 0) continue;
        /* The next character must be a separator, or the path IS the
         * directory.  Without this, "/tmpfile" would match "/tmp". */
        if (canon[dlen] == '/') return 1;
        if (canon[dlen] == '\0') return 0;   /* the directory itself, not a file */
    }
    return 0;
}
