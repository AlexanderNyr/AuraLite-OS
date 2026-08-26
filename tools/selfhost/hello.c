/* tools/selfhost/hello.c -- freestanding in-guest smoke source.
 *
 * SELFHOST_PLAN.md SH1: this file is compiled INSIDE AuraLite by the
 * guest toolchain (`run tcc -nostdlib -o /tmp/h /tests/selfhost_hello.c`),
 * linked by tcc's own ELF linker, and then executed in the guest.
 *
 * It uses basic asm only -- no libc, no crt0 -- so the whole pipeline is
 * exercised without depending on SH2 (libc link) yet:
 *   write(1, m, len); exit(0);
 *
 * The message IS the receipt contract of SELFHOST_PLAN.md §8
 * ("[selfhost] tcc PASS: ...") so the host-side integration case can
 * assert the exact greppable line.  44 bytes, kept in sync with the
 * literal below -- the mov $44,%rdx operand and the string are one fact.
 */
char m[] = "[selfhost] tcc PASS: 1 binary built and run\n";

void _start(void)
{
    __asm__("mov $1,%rax; mov $1,%rdi; lea m(%rip),%rsi; mov $44,%rdx;"
            " syscall; mov $60,%rax; xor %rdi,%rdi; syscall");
}
