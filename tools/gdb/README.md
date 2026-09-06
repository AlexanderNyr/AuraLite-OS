# tools/gdb — the AuraLite kernel debugging kit (RESIDUE2 T9)

Pretty-printers and convenience commands for GDB against
`build/kernel.elf` (built `-g`, so the DWARF is in the image).

## Quick start

```sh
make kernel
gdb -ex 'source tools/gdb/aura.gdb' build/kernel.elf
(gdb) print default_theme        # or: aura_default_theme
gui_theme {desktop_top=#102040, title_active=#2F60C0, ...}
```

Attaching to a running kernel (QEMU with the gdb stub):

```sh
qemu-system-x86_64 ... -s -S      # -s = gdbstub on :1234, -S = frozen
gdb -ex 'source tools/gdb/aura.gdb' \
    -ex 'target remote :1234' build/kernel.elf
```

## What you get

* **Pretty-printers** (automatic on `print`/`backtrace` displays):
  * `gui_theme_t` — one line, colors as `#RRGGBB`, geometry after;
  * `struct tcb` — `id name state rsp`, states as mnemonics
    (READY/RUNNING/BLOCKED/STOPPED/DEAD);
  * `struct ofd` — refcnt, position, path.
  Printers match by field signature and fall through to the raw
  display when a type does not match — they never hide data.
* **Commands**:
  * `aura_theme` — the ACTIVE desktop theme (`gui.c`'s `active_theme`;
    needs a live target or a core);
  * `aura_default_theme` — the const built-in (file-backed `.rodata`:
    works with no target at all);
  * `aura_windows` — the GUI window table: `wid owner rect flags title`.

## The gate

`tests/unit/test_gdb_scripts.sh` (runs under `make test-unit`): the
module compiles, `aura.gdb` loads against the real image, the default
theme renders through the printer (`#2F60C0` visible), `struct tcb`
resolves, and the commands degrade cleanly without a live inferior.
Skips (loudly, not red) where gdb or `build/kernel.elf` is absent;
the CI image installs gdb for exactly this gate.
