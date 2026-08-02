/* elfperm.c — userspace probe for strict ELF permissions and NX.
 *
 * Each mode deliberately triggers a user-mode protection fault when the kernel
 * enforces PT_LOAD permissions correctly. Reaching an "ELFPERM FAIL" line means
 * a security permission check regressed.
 */

#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"

static int text_target(void) {
    return 42;
}

static unsigned char data_code[] = { 0xC3 }; /* ret; must be RW but NX. */

typedef void (*void_fn_t)(void);

static int run_write_text(void) {
    volatile unsigned char *p = (volatile unsigned char *)(void *)&text_target;
    printf("ELFPERM: write-text begin target=%p\n", (void *)p);
    fflush(stdout);
    *p = (unsigned char)(*p ^ 0x01U);
    printf("ELFPERM FAIL: wrote to .text without SIGSEGV\n");
    fflush(stdout);
    return 1;
}

static int run_exec_data(void) {
    volatile void_fn_t fn = (void_fn_t)(void *)data_code;
    printf("ELFPERM: exec-data begin target=%p\n", (void *)data_code);
    fflush(stdout);
    fn();
    printf("ELFPERM FAIL: executed .data without SIGSEGV\n");
    fflush(stdout);
    return 1;
}

static int read_mode_file(char *buf, unsigned long cap) {
    int fd = open("/tmp/elfperm.mode", O_RDONLY);
    if (fd < 0) return -1;
    int64_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    for (int64_t i = 0; i < n; i++) {
        if (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' ' || buf[i] == '\t') {
            buf[i] = '\0';
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    char mode[32];
    const char *selected = 0;

    if (argc >= 2) {
        selected = argv[1];
    } else if (read_mode_file(mode, sizeof(mode)) == 0) {
        selected = mode;
    }

    if (!selected) {
        printf("ELFPERM: usage: /elfperm write-text|exec-data, or write /tmp/elfperm.mode\n");
        fflush(stdout);
        return 2;
    }
    if (strcmp(selected, "write-text") == 0) return run_write_text();
    if (strcmp(selected, "exec-data") == 0) return run_exec_data();
    printf("ELFPERM: unknown mode %s\n", selected);
    fflush(stdout);
    return 2;
}
