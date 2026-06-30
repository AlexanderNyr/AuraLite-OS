#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int fail(const char *msg) {
    printf("FIFOLINK FAIL: %s\n", msg);
    return 1;
}

static int pass(int cond, const char *msg) {
    if (!cond) return fail(msg);
    printf("FIFOLINK PASS: %s\n", msg);
    return 0;
}

int main(void) {
    /* ---- Named FIFO ---- */
    const char *fifo = "/tmp/np.fifo";
    unlink(fifo);
    if (mkfifo(fifo, 0644) != 0) return fail("mkfifo");

    struct stat fst;
    if (lstat(fifo, &fst) != 0) return fail("lstat fifo");
    if (pass((fst.st_type == ST_TYPE_FIFO), "mkfifo created FIFO node") != 0) return 1;

    int fd = open(fifo, O_RDWR, 0);
    if (fd < 0) return fail("open fifo");
    const char *msg = "fifo-bytes";
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg)) return fail("write fifo");
    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, strlen(msg));
    close(fd);
    if (pass((n == (ssize_t)strlen(msg) && strcmp(buf, msg) == 0),
             "FIFO round-trip read/write") != 0) return 1;

    /* ---- Symlink ---- */
    const char *target = "/tmp/realfile.txt";
    const char *link = "/tmp/link.txt";
    unlink(link);
    unlink(target);

    fd = open(target, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return fail("open target");
    const char *data = "linked-content";
    if (write(fd, data, strlen(data)) != (ssize_t)strlen(data)) return fail("write target");
    close(fd);

    if (symlink(target, link) != 0) return fail("symlink");

    char rl[64];
    memset(rl, 0, sizeof(rl));
    int64_t rn = readlink(link, rl, sizeof(rl) - 1);
    if (rn <= 0) return fail("readlink");
    rl[rn] = '\0';
    if (pass((strcmp(rl, target) == 0), "readlink returns target") != 0) return 1;

    struct stat lst, sst;
    if (lstat(link, &lst) != 0) return fail("lstat link");
    if (pass((lst.st_type == ST_TYPE_SYMLINK), "lstat sees symlink itself") != 0) return 1;

    if (stat(link, &sst) != 0) return fail("stat link follow");
    if (pass((sst.st_type == ST_TYPE_FILE && sst.st_size == (uint64_t)strlen(data)),
             "stat follows symlink to target") != 0) return 1;

    fd = open(link, O_RDONLY, 0);
    if (fd < 0) return fail("open through symlink");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (pass((n == (ssize_t)strlen(data) && strcmp(buf, data) == 0),
             "open/read follows symlink") != 0) return 1;

    printf("FIFOLINK PASS: fifo + symlink functional\n");
    return 0;
}
