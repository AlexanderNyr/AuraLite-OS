/* M6k: actual userspace FD/OFD lifecycle on AuraFS, not driver test pins. */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NAME "/data/m6k-file"
#define RENAMED "/data/m6k-renamed"
#define HARD "/data/m6k-hard"
#define MARKER "/data/m6k-result"
#define MARK "M6k-FD-PASS"

#define CHECK(name, condition) do { \
    if (!(condition)) { printf("[aurafs-fd] FAIL: %s errno=%d\n", name, errno); goto out; } \
    printf("[aurafs-fd] ok: %s\n", name); \
} while (0)

static int payload(int fd, const char *value) {
    char bytes[16]; size_t n = strlen(value);
    return lseek(fd, 0, SEEK_SET) == 0 && read(fd, bytes, sizeof(bytes)) == (ssize_t)n &&
           !memcmp(bytes, value, n);
}

int main(void) {
    int old = -1, alias = -1, fresh = -1, hard = -1, marker = -1, rc = 1;
    struct stat oldst, newst, st;
    char bytes[8];
    printf("[aurafs-fd] START: /data open/dup/fork/close lifecycle\n");
    unlink(NAME); unlink(RENAMED); unlink(HARD); unlink(MARKER);
    old = open(NAME, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("create old file", old >= 0 && write(old, "OLD", 3) == 3);
    CHECK("old inode identity", fstat(old, &oldst) == 0 && oldst.st_nlink == 1);
    alias = dup(old);
    CHECK("dup shares offset", alias >= 0 && lseek(alias, 0, SEEK_CUR) == 3);
    CHECK("unlink with open descriptors", unlink(NAME) == 0);
    fresh = open(NAME, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("recreate distinct inode", fresh >= 0 && fstat(fresh, &newst) == 0 && newst.st_inode != oldst.st_inode);
    CHECK("new payload", write(fresh, "NEW", 3) == 3);
    CHECK("old alias payload survives", payload(alias, "OLD"));
    CHECK("unlinked stat identity", fstat(alias, &st) == 0 && st.st_inode == oldst.st_inode && st.st_nlink == 0);
    CHECK("close first dup reference", close(old) == 0); old = -1;
    CHECK("write unlinked alias", lseek(alias, 0, SEEK_SET) == 0 && write(alias, "GONE", 4) == 4);
    CHECK("new file isolated", payload(fresh, "NEW"));
    CHECK("seek before fork", lseek(alias, 0, SEEK_SET) == 0);
    pid_t child = fork();
    CHECK("fork inherited descriptors", child >= 0);
    if (child == 0) {
        int ok = read(alias, bytes, 1) == 1 && bytes[0] == 'G';
        if (close(alias) != 0) ok = 0;
        /* _exit also closes inherited fresh; parent's OFD must survive. */
        _exit(ok ? 0 : 31);
    }
    int status = 0;
    CHECK("child exited cleanly", waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK("fork shares OFD offset", lseek(alias, 0, SEEK_CUR) == 1 && read(alias, bytes, sizeof(bytes)) == 3 && !memcmp(bytes, "ONE", 3));
    CHECK("parent survives child close", payload(fresh, "NEW"));
    /* Exercise actual VFS lookup/open/close under more than 64 inode names. */
    for (unsigned i = 0; i < 96; i++) {
        char path[64]; snprintf(path, sizeof(path), "/data/m6k-pressure-%u", i);
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0644);
        if (fd < 0) { printf("[aurafs-fd] FAIL: pressure open %u errno=%d\n", i, errno); goto out; }
        int ok = write(fd, "P", 1) == 1;
        if (close(fd) != 0) ok = 0;
        if (unlink(path) != 0) ok = 0;
        if (!ok) { printf("[aurafs-fd] FAIL: pressure I/O %u\n", i); goto out; }
    }
    CHECK("96 real open-close cycles", payload(alias, "GONE") && payload(fresh, "NEW"));
    CHECK("final old alias close", close(alias) == 0); alias = -1;
    CHECK("rename live file", rename(NAME, RENAMED) == 0);
    CHECK("create hard link", link(RENAMED, HARD) == 0);
    hard = open(HARD, O_RDWR);
    CHECK("hardlink same inode", hard >= 0 && fstat(hard, &st) == 0 && st.st_inode == newst.st_inode && st.st_nlink == 2);
    CHECK("hardlink visible size", lseek(hard, 0, SEEK_END) == 3 && write(hard, "!", 1) == 1 && payload(fresh, "NEW!"));
    CHECK("close original new OFD", close(fresh) == 0); fresh = -1;
    CHECK("remove both names", unlink(RENAMED) == 0 && unlink(HARD) == 0);
    CHECK("last open hardlink survives", payload(hard, "NEW!") && fstat(hard, &st) == 0 && st.st_nlink == 0);
    CHECK("fsync unlinked descriptor", fsync(hard) == 0);
    CHECK("final hardlink close", close(hard) == 0); hard = -1;
    errno = 0; int missing = open(RENAMED, O_RDONLY);
    if (missing >= 0) close(missing);
    CHECK("names stay absent", missing < 0 && errno == ENOENT);
    marker = open(MARKER, O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("persist completion marker", marker >= 0 && write(marker, MARK, sizeof(MARK) - 1) == sizeof(MARK) - 1 && fsync(marker) == 0);
    CHECK("marker roundtrip", payload(marker, MARK));
    CHECK("close marker", close(marker) == 0); marker = -1;
    printf("[aurafs-fd] PASS: actual open/dup/fork/close, unlink/recreate, hardlinks, pressure, fsync\n");
    rc = 0;
out:
    if (old >= 0) close(old);
    if (alias >= 0) close(alias);
    if (fresh >= 0) close(fresh);
    if (hard >= 0) close(hard);
    if (marker >= 0) close(marker);
    return rc;
}
