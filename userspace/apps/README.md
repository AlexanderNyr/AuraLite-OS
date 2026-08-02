# userspace/apps — applications

Programs a user runs on purpose. They ship into `/apps`, except for a handful
that live in `/bin` because they are part of the working system rather than
things one chooses to launch (`apm`, `play`, `sysinfo`, `hello`).

The `gui-*` directories are AuraGUI applications; the rest are terminal
programs.

An application here should:

- **exit on its own** if it is not interactive, or have an obvious way out if
  it is. Two integration tests have already been written and rewritten
  because `calc` and `editor` sat waiting for input and swallowed every
  command that followed.
- **not hardcode paths to other programs.** Use `prog_resolve()` from
  `<unistd.h>`; the search path is what let the F3 layout move happen without
  touching a single application.

Adding one means a compile rule and a link entry in the Makefile, and a name
in `INITRD_APPS` so it is packed.
