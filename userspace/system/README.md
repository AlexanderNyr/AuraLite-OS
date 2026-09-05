# userspace/system — programs the kernel starts by itself

Only programs the kernel or the boot path launches without being asked, and
which the system does not work without, belong here.

| Program | Ships as | Notes |
|---|---|---|
| `init` | `/bin/init` | x86_64 PID 1 and the interactive shell |
| `init32` | `/bin/init` (i386 image) | i386 PID 1 (I386_PLAN) |
| `initrv` | `/bin/init` (rv64 image) | riscv64 PID 1 (RISCV_PLAN) |
| `inita64` | `/bin/init` (a64 image) | aarch64 PID 1 (ARM64_PLAN) |
| `smallsh` | `/bin/smallsh` (32/rv/a64 images) | the shared bring-up shell: one portable C source compiled per bring-up arch (i386, riscv64, aarch64) through the `AURA_LIBC` seam — `libc32` / `libcrv` / `libca64` expose the same six syscalls behind their own crt0 and trap instruction |

`init` is special in a way worth knowing before editing it: it is embedded in
the **kernel image** as well as packed into the initrd. There are two build
sites, and changing only one of them produces a system that boots the old
shell with the new initrd and gives no hint why.

If you are adding a program, it almost certainly does not belong here. A
daemon that is started on demand is an application; a thing that probes the
system is a test.
