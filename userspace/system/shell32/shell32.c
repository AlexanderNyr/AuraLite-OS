/* userspace/system/shell32/shell32.c -- the i386 interactive shell
 * (I386_PLAN I7; the `auralite#` gate I5 deferred here, delivered).
 *
 * A Ring 3 program end to end: reads cooked lines through SYS_READ
 * (PS/2 keyboard or serial -- the kernel's console ring merges both),
 * runs initrd programs through SYS_SPAWN, prints through SYS_WRITE.
 * The command set is the honest subset of the 64-bit init shell that
 * has kernel support at I7; each absent command names the phase that
 * brings it rather than pretending.
 */

#include "lib/libc32/libc32.h"

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
    puts32("commands:\n");
    puts32("  help          this text\n");
    puts32("  uname         kernel identification\n");
    puts32("  pid           show this shell's pid\n");
    puts32("  echo <text>   print text\n");
    puts32("  run <path>    spawn an initrd ELF32 (e.g. run bin32/init32)\n");
    puts32("  exit          leave the shell\n");
    puts32("absent on purpose: ls/cat (VFS port, I8), net tools (I8),\n");
    puts32("gui (no framebuffer on the i386 path yet, I8)\n");
}

int main(void)
{
    puts32("\nAuraLite i386 shell (I7): type 'help'\n");

    char line[128];

    for (;;) {
        puts32("auralite# ");

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
            puts32("AuraLite OS i386 (protected mode, higher half, "
                   "I386_PLAN I7)\n");
        } else if (str_eq(line, "pid")) {
            puts32("pid ");
            print_dec(getpid());
            puts32("\n");
        } else if (starts_with(line, "echo ")) {
            puts32(line + 5);
            puts32("\n");
        } else if (str_eq(line, "echo")) {
            puts32("\n");
        } else if (starts_with(line, "run ")) {
            long code = spawn(line + 4);
            if (code < 0) {
                puts32("run: failed (not found / not ELF32 / refused)\n");
            } else {
                puts32("exit code ");
                print_dec(code);
                puts32("\n");
            }
        } else if (str_eq(line, "exit")) {
            puts32("bye\n");
            return 0;
        } else {
            puts32(line);
            puts32(": unknown command (try 'help')\n");
        }
    }
}
