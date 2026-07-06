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
#include "libc/include/errno.h"

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
    int            fd;              /* file descriptor (for dirfd)        */
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

int dirfd(DIR *dirp) {
    if (!dirp) { errno = EINVAL; return -1; }
    if (dirp->fd < 0) { errno = ENOSYS; return -1; }
    return dirp->fd;
}

int scandir(const char *dir, struct dirent ***namelist,
            int (*sel)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **)) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0, cap = 16;
    struct dirent **list = malloc((size_t)cap * sizeof(*list));
    if (!list) { closedir(d); return -1; }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (sel && !sel(ent)) continue;
        if (n == cap) {
            cap *= 2;
            struct dirent **nl = realloc(list, (size_t)cap * sizeof(*list));
            if (!nl) {
                for (int i = 0; i < n; i++) free(list[i]);
                free(list);
                closedir(d);
                return -1;
            }
            list = nl;
        }
        list[n] = malloc(sizeof(*ent));
        if (!list[n]) break;
        memcpy(list[n], ent, sizeof(*ent));
        n++;
    }
    closedir(d);
    if (compar)
        qsort(list, (size_t)n, sizeof(*list),
              (int (*)(const void *, const void *))compar);
    *namelist = list;
    return n;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int versionsort(const struct dirent **a, const struct dirent **b) {
    return strverscmp((*a)->d_name, (*b)->d_name);
}
