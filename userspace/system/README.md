# userspace/system — programs the kernel starts by itself

Exactly one thing belongs here: a program the kernel or the boot path launches
without being asked, and which the system does not work without.

| Program | Ships as | Notes |
|---|---|---|
| `init` | `/bin/init` | PID 1 and the interactive shell |

`init` is special in a way worth knowing before editing it: it is embedded in
the **kernel image** as well as packed into the initrd. There are two build
sites, and changing only one of them produces a system that boots the old
shell with the new initrd and gives no hint why.

If you are adding a program, it almost certainly does not belong here. A
daemon that is started on demand is an application; a thing that probes the
system is a test.
