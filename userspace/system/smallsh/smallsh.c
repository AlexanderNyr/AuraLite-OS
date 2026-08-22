/* userspace/system/smallsh/smallsh.c -- the shared bring-up shell
 * (I386_PLAN I7 as shell32.c; promoted to two-arch source in
 * RISCV_PLAN V5).
 *
 * One shell source, two bring-up arches: pure portable C over the
 * tiny libc surface, compiled per-arch with that arch's crt0 and
 * syscall wrapper.  The libc seam is AURA_LIBC, defined on the
 * command line: lib/libc32/libc32.h for i386, lib/libcrv/libcrv.h
 * for rv64.  Both headers expose the same names for the same six
 * syscalls (the D4 one-table rule made this promotion a rename, not
 * a port -- read/write/spawn/getpid/exit are the same *numbers*
 * everywhere, only the trap instruction differs).
 *
 * The per-arch strings (uname's identity line, run's example path)
 * come through the same seam -- everything else is identical down to
 * the bytes of the prompt, and the i386 smoke family is the
 * regression gate proving the promotion changed nothing there.
 */

#include AURA_LIBC

/* Per-arch identity, supplied next to AURA_LIBC. */
#ifndef AURA_UNAME
#error "AURA_UNAME must be defined (the uname line for this arch)"
#endif
#ifndef AURA_RUN_EXAMPLE
#error "AURA_RUN_EXAMPLE must be defined (run's example path)"
#endif

/* libc32 spells its helpers strlen32/puts32; libcrv strlen_rv/puts_rv.
 * The shell uses one local name for each -- the seam stays in the
 * headers, not sprinkled through the code. */
#ifndef sh_puts
#define sh_puts AURA_PUTS
#endif

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int starts_with(const char *s, const char *pfx)
{
    while (*pfx) {
        if (*s++ != *pfx++)
            return 0;
    }
    return 1;
}

static void print_dec(long v)
{
    char buf[12];
    int i = 0;
    if (v < 0) { write(1, "-", 1); v = -v; }
    do { buf[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i--)
        write(1, &buf[i], 1);
}

static void help(void)
{
    sh_puts("commands:\n");
    sh_puts("  help          this text\n");
    sh_puts("  uname         kernel identification\n");
    sh_puts("  pid           show this shell's pid\n");
    sh_puts("  echo <text>   print text\n");
    sh_puts("  run <path>    spawn an initrd ELF (e.g. run " AURA_RUN_EXAMPLE ")\n");
    sh_puts("  ls [path]     list a directory (P4: through open/readdir)\n");
    sh_puts("  cat <path>    print a file (P4: open/lseek/read)\n");
    sh_puts("  stat <path>   size and kind (P4: stat)\n");
    sh_puts("  exit          leave the shell\n");
    sh_puts("absent on purpose: net tools, gui (no framebuffer on\n");
    sh_puts("this bring-up path)\n");
}

int main(void)
{
    sh_puts("\nAuraLite shell (smallsh): type 'help'\n");

    char line[128];

    for (;;) {
        sh_puts("auralite# ");

        long n = read(0, line, sizeof(line) - 1);
        if (n <= 0)
            continue;
        /* Strip the newline, NUL-terminate. */
        if (line[n - 1] == '\n')
            n--;
        line[n] = '\0';
        if (n == 0)
            continue;

        if (str_eq(line, "help")) {
            help();
        } else if (str_eq(line, "uname")) {
            sh_puts(AURA_UNAME "\n");
        } else if (str_eq(line, "pid")) {
            sh_puts("pid ");
            print_dec(getpid());
            sh_puts("\n");
        } else if (starts_with(line, "echo ")) {
            sh_puts(line + 5);
            sh_puts("\n");
        } else if (str_eq(line, "echo")) {
            sh_puts("\n");
        } else if (starts_with(line, "run ")) {
            long code = spawn(line + 4);
            if (code < 0) {
                sh_puts("run: failed (not found / not our ELF / refused)\n");
            } else {
                sh_puts("exit code ");
                print_dec(code);
                sh_puts("\n");
            }
        } else if (str_eq(line, "ls") || starts_with(line, "ls ")) {
            const char *path = str_eq(line, "ls") ? "/" : line + 3;
            long fd = open(path, 0);
            if (fd < 0) {
                sh_puts("ls: cannot open (no fs mounted, or no such dir)\n");
            } else {
                struct aura_dirent de;
                long i, got = 0;
                for (i = 0; readdir((int)fd, i, &de) > 0; i++) {
                    sh_puts("  ");
                    sh_puts(de.name);
                    if (de.is_dir)
                        sh_puts("/");
                    sh_puts("\n");
                    got++;
                }
                if (!got)
                    sh_puts("  (empty)\n");
                close((int)fd);
            }
        } else if (starts_with(line, "cat ")) {
            long fd = open(line + 4, 0);
            if (fd < 0) {
                sh_puts("cat: cannot open\n");
            } else {
                /* lseek round-trip first: size via SEEK_END, rewind. */
                long size = lseek((int)fd, 0, AURA_SEEK_END);
                lseek((int)fd, 0, AURA_SEEK_SET);
                char buf[64];
                long r;
                while ((r = read((int)fd, buf, sizeof(buf))) > 0)
                    write(1, buf, (unsigned long)r);
                sh_puts("\n(");
                print_dec(size);
                sh_puts(" bytes)\n");
                close((int)fd);
            }
        } else if (starts_with(line, "stat ")) {
            struct aura_stat st;
            if (stat(line + 5, &st) < 0) {
                sh_puts("stat: failed\n");
            } else {
                sh_puts(st.is_dir ? "dir, " : "file, ");
                print_dec((long)st.size);
                sh_puts(" bytes\n");
            }
        } else if (str_eq(line, "exit")) {
            sh_puts("bye\n");
            return 0;
        } else {
            sh_puts(line);
            sh_puts(": unknown command (try 'help')\n");
        }
    }
}
