/*
 * test_permissions.c — host-side unit test for POSIX file permission checks.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "libc/include/unistd.h"

struct mock_vnode {
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
};

struct mock_tcb {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t supplementary_gids[32];
    int ngroups;
};

static int mock_check_perm(struct mock_vnode *vn, int access, struct mock_tcb *tcb) {
    if (!vn) return -ENOENT;
    if (access == 0) return 0;
    if (!tcb || tcb->euid == 0) return 0; /* root always passes */

    uint16_t mode = vn->mode & 0777;
    int granted = 0;
    if (tcb->euid == vn->uid) {
        if (mode & 0400) granted |= 4;
        if (mode & 0200) granted |= 2;
        if (mode & 0100) granted |= 1;
    } else {
        int in_group = (tcb->egid == vn->gid);
        if (!in_group) {
            for (int i = 0; i < tcb->ngroups; i++) {
                if (tcb->supplementary_gids[i] == vn->gid) { in_group = 1; break; }
            }
        }
        if (in_group) {
            if (mode & 0040) granted |= 4;
            if (mode & 0020) granted |= 2;
            if (mode & 0010) granted |= 1;
        } else {
            if (mode & 0004) granted |= 4;
            if (mode & 0002) granted |= 2;
            if (mode & 0001) granted |= 1;
        }
    }
    if ((granted & access) == access) return 0;
    return -EACCES;
}

static int failures = 0;
#define CHECK(cond) do { \
    if (cond) printf("PASS: %s\n", #cond); \
    else { printf("FAIL: %s\n", #cond); failures++; } \
} while(0)

int main(void) {
    struct mock_vnode vn = { 0600, 1000, 1000 };
    struct mock_tcb root = { 0, 0, 0, 0, 0, 0, {0}, 0 };
    struct mock_tcb owner = { 1000, 1000, 1000, 1000, 1000, 1000, {0}, 0 };
    struct mock_tcb other = { 1001, 1001, 1001, 1001, 1001, 1001, {0}, 0 };

    /* Root passes on 0600 */
    CHECK(mock_check_perm(&vn, R_OK | W_OK, &root) == 0);
    CHECK(mock_check_perm(&vn, X_OK, &root) == 0);

    /* Owner passes R/W, fails X */
    CHECK(mock_check_perm(&vn, R_OK, &owner) == 0);
    CHECK(mock_check_perm(&vn, W_OK, &owner) == 0);
    CHECK(mock_check_perm(&vn, X_OK, &owner) == -EACCES);

    /* Other fails all */
    CHECK(mock_check_perm(&vn, R_OK, &other) == -EACCES);
    CHECK(mock_check_perm(&vn, W_OK, &other) == -EACCES);

    /* Group test */
    vn.mode = 0640;
    struct mock_tcb grp = { 1002, 1002, 1002, 1000, 1000, 1000, {0}, 0 };
    CHECK(mock_check_perm(&vn, R_OK, &grp) == 0);
    CHECK(mock_check_perm(&vn, W_OK, &grp) == -EACCES);

    /* Supplementary group test */
    struct mock_tcb supp = { 1003, 1003, 1003, 2000, 2000, 2000, {1000, 3000}, 2 };
    CHECK(mock_check_perm(&vn, R_OK, &supp) == 0);

    if (failures == 0) {
        printf("test_permissions: ALL PASS\n");
        return 0;
    }
    printf("test_permissions: %d FAILURE(S)\n", failures);
    return 1;
}