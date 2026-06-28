/* libc/src/dirent.c — opendir / readdir (P10) */

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct __dirstream {
    int fd;
    struct dirent entry;
    int pos;
};

DIR *opendir(const char *name) {
    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        return NULL;
    }
    dir->fd = fd;
    dir->pos = 0;
    return dir;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return NULL;

    struct vfs_dirent kdent;
    int n = readdir((const char *)dirp->fd, &kdent, 1);   /* reuse syscall 80 */
    if (n <= 0) return NULL;

    dirp->entry.d_ino = kdent.inode;
    dirp->entry.d_type = kdent.type;
    strncpy(dirp->entry.d_name, kdent.name, 255);
    dirp->entry.d_name[255] = 0;
    return &dirp->entry;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    close(dirp->fd);
    free(dirp);
    return 0;
}