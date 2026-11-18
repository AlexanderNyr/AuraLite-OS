# RBFI — vendored upstream sources

This directory contains [RBFI](https://github.com/Syscall1dev/RBFI) as
vendored by AuraLite OS for issue #16.

| File | Origin |
|---|---|
| `RBFI_x86.nas` | upstream `RBFI_x86.nas`, commit `500dae1` ("added new script in CPUID no_found_proccesor", 2026-11-18), with the three fixes below |
| `LICENSE` | upstream `LICENSE`, verbatim (MIT, © 2026 $yscall) |
| `rbfi_stage2.asm` | **AuraLite's own** adapter stage — not upstream |
| `README.md` | this file |

Upstream: <https://github.com/Syscall1dev/RBFI> · MIT · `$yscall` (Syscall1dev).

## Why the vendored copy differs from upstream

Upstream `RBFI_x86.nas` **does not assemble**. `nasm -f bin RBFI_x86.nas`
stops at line 745:

```
RBFI_x86.nas:745: error: label or instruction expected at start of line
```

Three defects, and only these three, are fixed here:

| Line (upstream) | Defect | Fix |
|---|---|---|
| 745 | a stray `0` on its own line, between the second packet setup and the doorbell | removed |
| 782 | `htl` instead of `hlt` inside `launch32bit` | corrected |
| 832 | `gdt_poiii: dd compat322` — `compat322` exists only inside the commented-out 32-bit rollback block, so the symbol is undefined | the `dd` is zeroed; the stub is never the operand of an `lgdt`/`ljmp` |

No behaviour in RBFI's boot path is changed: the third fix touches a
far-pointer stub for a feature (`launch32bit`) that upstream itself leaves as
a `hlt` loop.

After the fixes the ROM assembles to **exactly 131072 bytes** with the reset
vector `EA 00 00 00 E0` in its last 16 bytes, which is what makes
`qemu-system-x86_64 -bios build/rbfi/RBFI.bin` land at `0xFFFE0000`.

`tools/check_rbfi.py` re-assembles this file on every run and compares the
result against the sizes above, so a future upstream fix cannot silently turn
one of these patches into a no-op.

## What AuraLite does with it

RBFI is **not** the default boot path. `make iso` is unchanged and does not
build anything in this directory. See [`docs/rbfi.md`](../../docs/rbfi.md)
for the full account, including why RBFI cannot run under QEMU.
