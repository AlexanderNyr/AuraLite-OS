/* kernel/fs/path.c — the implementation for path.h (RESIDUE2 T4).
 *
 * Host-testable: no kernel includes, no allocation, no globals.  Every rule
 * this file implements has a named case in tests/unit/test_vfs_path.c; if
 * you add a rule here, add the case there in the same commit.
 */

#include "kernel/fs/path.h"

int vfs_canonical_path(const char *in, char *out, size_t out_len) {
    if (!in || !out || out_len < 2) goto refuse;
    if (in[0] != '/') goto refuse;        /* absolute paths only */

    size_t w = 0;                         /* out is written [0..w), NUL at w */
    out[w++] = '/';

    const char *p = in;
    while (*p) {
        while (*p == '/') p++;            /* skip separator(s) */
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.') continue;          /* "." */

        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* Climb one component; at the root this is absorbed.  w must
             * land BEFORE the cancelled component's separator so the next
             * append reuses that slot ("/tmp/sub/../a" -> "/tmp/a", not
             * "/tmp//a"). */
            if (w > 1) {
                while (w > 1 && out[w - 1] != '/') w--;   /* to the separator */
                if (w > 1) w--;                           /* drop it too */
            }
            continue;
        }

        if (w > 1) {                      /* need a '/' before the component */
            if (w + 1 >= out_len) goto refuse;
            out[w++] = '/';
        }
        if (w + clen >= out_len) goto refuse;
        for (size_t i = 0; i < clen; i++) out[w + i] = start[i];
        w += clen;
    }

    out[w] = '\0';
    return 0;

refuse:
    if (out && out_len) out[0] = '\0';
    return -1;
}
