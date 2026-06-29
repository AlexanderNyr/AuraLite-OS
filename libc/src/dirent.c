/* libc/src/dirent.c — POSIX opendir / readdir / closedir / rewinddir (P10)
 *
 * AuraLite's kernel exposes directory listing through the raw aura_readdir()
 * wrapper (SYS_LISTDIR == 80), which fills an array of struct aura_dirent in a
 * single call.  We snapshot the whole directory at opendir() time and hand the
 * entries out one at a time from readdir(), which is sufficient for the typical
 * read-only directory traversal POSIX programs perform.
 */

#include "libc/include/dirent.h"
#include "libc/include/unistd.h"
#include "libc/include/stdlib.h"
#include "libc/include/string.h"

#define DIRENT_MAX 256   /* entries snapshotted per directory */

/* AuraLite VFS type codes (kernel/fs/vfs.h: FILE=1, DIR=2, CHARDEV=3, SYMLINK=4). */
static unsigned char vfs_type_to_dt(unsigned int t) {
    switch (t) {
    case 1: return DT_REG;
    case 2: return DT_DIR;
    case 3: return DT_CHR;
    case 4: return DT_LNK;
    default: return DT_UNKNOWN;
    }
}

struct __dirstream {
    struct dirent  entry;          /* returned by the last readdir()      */
    int            count;          /* number of valid entries             */
    int            pos;            /* next entry to return                */
    struct aura_dirent ents[DIRENT_MAX];
};

DIR *opendir(const char *name) {
    if (!name) return NULL;

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) return NULL;

    int n = aura_readdir(name, dir->ents, DIRENT_MAX);
    if (n < 0) {
        free(dir);
        return NULL;
    }
    dir->count = n;
    dir->pos   = 0;
    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->pos >= dirp->count) return NULL;

    struct aura_dirent *k = &dirp->ents[dirp->pos++];
    dirp->entry.d_ino  = (ino_t)k->inode;
    dirp->entry.d_type = vfs_type_to_dt(k->type);
    strncpy(dirp->entry.d_name, k->name, sizeof(dirp->entry.d_name) - 1);
    dirp->entry.d_name[sizeof(dirp->entry.d_name) - 1] = '\0';
    return &dirp->entry;
}

void rewinddir(DIR *dirp) {
    if (dirp) dirp->pos = 0;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    free(dirp);
    return 0;
}
