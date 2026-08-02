#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

static int fail(const char *msg) {
    printf("TIMESTEST FAIL: %s\n", msg);
    return 1;
}

static int expect(int cond, const char *msg) {
    if (!cond) return fail(msg);
    printf("TIMESTEST PASS: %s\n", msg);
    return 0;
}

int main(void) {
    const char *path = "/tmp/timestest.txt";
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return fail("open create");
    close(fd);

    struct stat st0, st1, st2, st3;
    if (stat(path, &st0) != 0) return fail("stat after create");
    if (expect(st0.st_mtime != 0 && st0.st_ctime != 0 && st0.st_atime != 0,
               "create populated mtime/ctime/atime") != 0) return 1;

    sleep(1);
    fd = open(path, O_WRONLY, 0);
    if (fd < 0) return fail("open write");
    const char *msg = "timestamp data";
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg)) return fail("write data");
    close(fd);

    if (stat(path, &st1) != 0) return fail("stat after write");
    if (expect(st1.st_mtime >= st0.st_mtime && st1.st_ctime >= st0.st_ctime,
               "write advanced mtime/ctime") != 0) return 1;

    sleep(1);
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) return fail("open read");
    char buf[32];
    if (read(fd, buf, sizeof(buf)) <= 0) return fail("read data");
    close(fd);

    if (stat(path, &st2) != 0) return fail("stat after read");
    if (expect(st2.st_atime >= st1.st_atime, "read advanced atime") != 0) return 1;

    sleep(1);
    if (truncate(path, 4) != 0) return fail("truncate");
    if (stat(path, &st3) != 0) return fail("stat after truncate");
    if (expect(st3.st_size == 4 && st3.st_mtime >= st2.st_mtime && st3.st_ctime >= st2.st_ctime,
               "truncate updated size and mtime/ctime") != 0) return 1;

    printf("TIMESTEST PASS: timestamps functional\n");
    return 0;
}
