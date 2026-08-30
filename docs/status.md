# AuraLite OS Status Matrix

This document describes the current state of the repository. It is more current
than the historical 14-phase roadmap in `PLAN.md`.

Legend:

- ✅ **Implemented / exercised** — built by default and has a boot-time or
  host-side test path.
- 🧪 **Experimental** — code exists and may work in constrained scenarios, but
  semantics or coverage are incomplete.
- 🚧 **WIP / partial** — scaffolding or protocol code exists; full data path is
  not complete.
- ❌ **Not implemented** — no working support yet.

## Performance (OPT_PLAN.md, phases O0–O9)

The optimization plan's ledger lives in `OPT_PLAN.md` §6; the live
counters are `/proc/perf` (names are an interface — `test_perf_smoke`
parses them).  Headline numbers, all measured under QEMU/TCG on the
plan's reference machine:

| What | Before | After |
|---|---:|---:|
| memcpy 64 KiB (guest, TCG) | 11 MB/s | 82 MB/s |
| memset 1 MiB (guest, TCG) | 342 MB/s | 1 687 MB/s |
| boot → shell, default (`selftest=fast`) | ~5.1 s | ~4.0 s |
| serial log carried by | per-byte busy-wait | 16 KiB TX ring + THRE IRQ (97% of bytes) |
| compositor work per 1 Hz clock frame (UEFI) | 1 024 000 px (full screen) | 94 805 px (dirty union) |
| TLB shootdown | full CR3 reload, broadcast | invlpg range, addressed + CR3-filtered |
| idle busy% at the shell | 36.2 | 0.3 |
| initrd.tar | 8 806 400 B | 3 061 760 B (−65%, `--gc-sections`) |

Honesty notes carried from the plan: QEMU serial is effectively
infinite-baud, so O3's wall-clock win is real-hardware-shaped (the
counters are the in-QEMU proof); Fact 5's kmalloc walk number was the
heap self-test measuring itself (a real boot walks ~63–80 nodes — the
O6 size-class cache is infrastructure for runtime churn, not a boot
fix); PAT/WC framebuffer mapping remains recorded real-hardware
residue (the R11 metal package carries its receipt slot), and PCID is
IMPLEMENTED since RESIDUE R11 — TCG-inert, host-tested, waiting on
the user's WHPX machine for its D-PCID-5 numbers.
`tools/check_opt_claims.py` (CI) keeps the plan's checkboxes
tied to the tree.

## Boot and CPU

| Feature | Status | Notes |
|---|---:|---|
| Custom BIOS + UEFI ISO boot | ✅ | Own loader chain (`boot/`, phases BL1–BL8); no third-party bootloader. One hybrid GPT+MBR image boots on SeaBIOS and OVMF from identical bytes. |
| Higher-half kernel | ✅ | Linked at `0xFFFFFFFF80100000`. |
| GDT / IDT / PIC | ✅ | 256 IDT gates, PIC IRQ remap. |
| TSS | ✅ | RSP0 works and is per-CPU.  IST1 is PMM-backed, guard-paged and ARMED on #DF (`idt_set_ist(8, 1)`, FIX_R1) with a live gate: `test_ist_double_fault.sh` proves the survival lane AND the A/B negative (`-DAURALITE_UNARM_IST` reproduces the pre-fix triple fault).  This row said "allocated but unused" long after FIX_R1 landed — corrected by the residue ledger's R1 pass (RES-05: the debt was in the DOC, not the tree). |
| Fatal STOP screen | ✅ | A kernel-mode fault paints a blue screen with a named STOP (`0x0000000E` `PAGE_FAULT`, `0x00000008` `DOUBLE_FAULT`, software `0x00001xxx`). The lock-free serial dump still goes first (`[diag]` then `[bsod] STOP=`). User-mode faults stay POSIX signals. Table and meanings: [`bsod.md`](bsod.md). Host `test_bsod`; QEMU greps on `test_panic_diag.sh` / `test_ist_double_fault.sh`. |
| SYSCALL/SYSRET | ✅ | Linux-like register ABI, custom syscall table. |
| Win32 personality (`w32`) | 🧪 | WIN32_PLAN W32-0 – W32-8. A mingw-w64-built PE32+ `.exe` runs unmodified: the kernel loads PE by magic, the shell routes an importing binary through `/apps/w32run`, which binds imports and runs the CRT startup. 44 functions across KERNEL32/USER32/GDI32; USER32/GDI32 map onto the native compositor. `LoadLibrary`/`GetProcAddress` load real user DLLs. **Experimental, and the gaps are deliberate**: SEH is a `setjmp` shim, not table-driven unwinding (C++ destructors do not run while unwinding), TLS is per-process, and registry/COM/DirectX/WinSock are not implemented. See [`win32.md`](win32.md). |
| SMP bring-up | ✅ | H8 scheduler: all online CPUs run preemptive round-robin with per-CPU run queues, LAPIC-timer ticks, cross-CPU work stealing and IPI TLB shootdown. **M1 added eager `FXSAVE`/`FXRSTOR` FPU/SSE context switching**, which H8 needed to be correct (gltest now passes 373/373 under `-smp 4`; `test_fpu_smp.sh`). |
| LAPIC / IOAPIC | 🧪 | LAPIC enable + timer on each CPU are implemented; IOAPIC routing is still future work and legacy PIC/PIT paths remain available. |

## Memory management

| Feature | Status | Notes |
|---|---:|---|
| Physical memory manager | ✅ | Bitmap over 4 KiB frames from the `boot_info_t` memmap (BIOS E820 / UEFI memory map). |
| Virtual memory manager | ✅ | Adopts and extends the bootloader's page tables through HHDM. |
| Kernel heap | ✅ | First-fit allocator with coalescing. |
| Per-process PML4 | 🧪 | Implemented for spawned user processes. |
| Copy-on-write | ✅ | `fork` via `paging_clone_user_space()` performs mark-and-share COW: page-table pages are copied, writable user frames are shared (write-protected in both parent and child) via `PAGE_FLAG_COW`; first write triggers `paging_handle_cow_fault()` to copy. PMM refcount (reference counting for shared frames) is implemented in `pmm.c`. |
| User pointer validation | 🧪 | `validate_user_range`, `copy_from_user`, `copy_to_user` use a #PF fixup path so TOCTOU/unmap during copy returns an error instead of panicking. |
| User ELF segment permissions / NX | ✅ | User linker emits page-aligned RX/R/NX/RW/NX `PT_LOAD` segments; `elf_load()` maps `PF_W` only as writable and applies `PAGE_FLAG_NO_EXEC` to non-`PF_X` segments. User stacks are writable+NX with guard pages left unmapped. `/elfperm` + `test_elf_permissions.sh` cover write-to-text and execute-from-data faults. |
| Stack guard pages + overflow diagnosis | ✅ | Kernel-thread stacks are bracketed by unmapped guard pages on both sides of each slot; user stacks have an unmapped guard page below them. `kernel/proc/guard.c` classifies a `#PF` on a known guard region and the `#PF` handler reports `[GUARD] kernel/user stack overflow`: a kernel-stack guard hit is fatal (`kernel_halt()`), a user-stack guard hit becomes SIGSEGV. `/stackguard` + `test_stack_guard.sh` and a host unit test cover the user overflow path and the classification boundaries. |
| Slab allocator | ✅ | `kernel/mm/slab.c` provides fixed-size caches; `tcb_cache`, `ofd_cache`, and `vnode_cache` are initialized during boot and used by thread/VFS allocation paths. |

## Scheduling and processes

| Feature | Status | Notes |
|---|---:|---|
| Kernel threads | ✅ | 16 KiB kernel stacks. |
| Preemptive round-robin | ✅ | PIT tick, quantum-based scheduling. |
| Blocking I/O / wait queues | ✅ | `kernel/proc/wait_queue.c` backs blocking pipes, futex waits, `select()`, and `nanosleep()` without yield-polling loops. |
| Ring 3 user mode | ✅ | ELF entry via `iretq`. |
| ELF loader | ✅ | Loads PT_LOAD segments at linked virtual addresses. |
| `spawn` | 🧪 | Used by shell `run <prog>` and integration-tested. |
| **Self-hosting toolchain** | 🧪 | SELFHOST_PLAN.md SH1+SH2+SH3: guest TinyCC (`make selfhost-deps selfhost-tcc`) compiles, links and runs AuraLite's own userland in-guest (`/bin/tcc`, sources under `/src` in the initrd; `test_selfhost_tcc.sh` + `test_selfhost_userland.sh` in the `selfhost` shard), and SH3's `aulink` (`tools/aulink/aulink.c`) links ELF in-guest without `ld.lld` (`test_aulink.sh` host parity). `SPAWN_MAX_IMAGE` 16 MiB, user stack 4 MiB, tmpfs 256 files/volume, malloc 16-aligned, shell MAX_ARGS 32 / INPUT_MAX 512 — all raised/fixed for the compiler workload. SH4 (the in-guest assembler) is split into SH4a–SH4e along the output-format gradient; SH4a+SH4b+SH4c+SH4d+SH4e landed (SH4 complete) — `tools/mini-asm/mini-asm.c` assembles all four `-f bin` boot files byte-identically to nasm, all 13 `-f elf64` and all 7 `-f elf32` kernel/libc objects to readelf parity (sections/symtab/relocs + section bytes): full SIB/segment-override/0x67 effective-address model, 16/32/64-bit modes, `default rel` → RIP-relative with R_X86_64_PC32/64, ELF32 SHT_REL with always-relocated absolute refs, REX.B/X for r8+ bases/indexes, `%include`/`%if`(with `&&`/`||`)/`%define`/`%assign`/`%macro`/`%rep`/`%+` preprocessor, and the in-guest run: the guest tcc builds mini-asm (`/src/mini-asm.c`) which assembles the boot-critical sources byte-identical to the host refs (`tests/unit/test_asm_parity.sh` `[selfhost] asm PASS (bin): 4/4` + `(elf64): 13/13` + `(elf32): 7/7`; `tests/integration/cases/test_selfhost_asm.sh` `[selfhost] asm PASS: 3/3 objects byte-identical`; kernels with mini-asm-built asm objects link and boot to the shell on both x86_64 and i386); SH5 split into SH5a–SH5d with **SH5a landed** — the spike: a minimal kernel (the real `boot.asm` via mini-asm + `tools/selfhost/spike/kmain.c` via a host tcc built from the same mob source the guest uses) linked by aulink against `kernel.ld` packs into the dual-boot ISO and **boots in QEMU**, printing `[selfhost] SH5a spike: tcc+aulink kernel booted at the higher half`; measured: tcc emits only PC32/PLT32/64 relocations (no 32-bit absolutes → `-mcmodel=kernel` unnecessary), `kernel.ld` maps tcc's `.data.ro` into rodata, aulink's expressions gained `|`/`&` (`tests/unit/test_sh5_spike.sh` + `tests/integration/cases/test_selfhost_kernel_spike.sh`, receipt `[selfhost] spike PASS: tcc+aulink kernel links at the higher half`); SH5b landed — aulink's kernel.ld layout now matches ld.lld on the real kernel objects: SHF_MERGE/SHF_STRINGS pools (strings AND .cst4/16/32 constants, per-section-name like lld, with relocation re-basing incl. the PC-relative -4 bias), section-start alignment by max input align, and the `__bss_start`/`__bss_end`-inside-block fix (`cur_addr` tracks `o->addr+o->size`); `tests/unit/test_sh5b_layout.sh` (17/17, in `make test-unit`): entry, 3 PT_LOADs `R E/R/RW`, `.text` addr+size 1:1, `.data`/`.bss` sizes 1:1, `.rodata` addr + ≤0x10 size delta (documented merge-order), identical `.text` symbol addresses, ordered `__bss_*`; receipt `[selfhost] layout PASS: aulink kernel.ld layout matches ld.lld`; **SH5c landed** — tcc compiles all 126 kernel C files, mini-asm the 9 `.asm` files, aulink links `build/selfhost/kernel-tcc.elf` (`make selfhost-kernel-tcc`), and the tcc-built kernel BOOTS to the Ring 3 shell (+2.7% boot-to-shell vs clang under TCG; `selftest=full` 19 PASS / 0 FAIL): tcc's packed-attribute gap is carried by `__TINYC__`-guarded `#pragma pack(push,1)` wraps (119 packed structs, sizeof parity clang↔tcc machine-checked), `__sync_*`/`__atomic_*_n` map through `kernel/lib/atomic_compat.h` + tcc's `<stdatomic.h>`, STAC/CLAC/RDRAND/RDSEED emit from byte encodings, the TCB's FXSAVE area is a `long double` union (tcc ignores member `aligned(16)`), thread stacks grew 16→32 KiB (tcc frames ~4x clang's), both loaders map a 6-MiB kernel window, and aulink's `.got` insertion now shifts the script's `__bss_*` symbols with `.bss` (the tcc kernel's 295 GOTPCRELs; the clang build is byte-identical) (`tests/unit/test_sh5c_kernel_tcc.sh` 13 assertions, receipt `[selfhost] sh5c PASS: tcc compiles the kernel; aulink links it at the higher half`; `tests/integration/cases/test_selfhost_kernel_tcc.sh` 19/19 boots the tcc kernel to the shell); **SH5d landed** — `tests/integration/cases/test_selfhost_kernel_guest.sh` (registered in the selfhost shard) prompt-synchronises 167 guest commands: guest `/bin/tcc` compiles all 126 x86_64 kernel C sources, guest-tcc-built `mini-asm` emits all 9 kernel `.asm` objects, and guest-tcc-built `aulink` lexically expands the two object directories and writes `/fat/KERNEL.ELF`; portable C output-path generators replace — and delete — the Python-only kernel header path (`check_selfhost_claims.py` now fails if a `.py` emitter returns while SH5 is ✅), and 1,024-file/128-dir initrd metadata carries the 720-file/78-dir source closure. The host extracts that exact 1,220,552-byte FAT artifact at LBA 64 (`e_entry == _start == 0xffffffff801bbbe0`), packs it only for the terminal second boot, and gets the normal Ring 3 shell / `uname` / `sysinfo` receipts plus `[perf] boot-to-shell: 391 ticks` — **26/26 assertions passed** (including the terminal receipt emitted by the second guest shell, the explicit 1,220,552 B ≤ 4,144,640 B FAT payload measurement, and a transport receipt for all 167 prompt-gated commands). The prompt-aware transport also refuses to continue past a `run` that did not exit zero, and is unit-tested against a stub guest (`test_prompt_qemu.sh`), as is the new `memchr` as the shipped body (`test_string_ext.c`, mutation-checked). Receipt `[selfhost] kernel PASS: tcc-built kernel booted to shell`. **SH6 split into SH6a–SH6f, SH6a landed** — the x86_64 shell (`userspace/system/init/init.c`, not `smallsh`, which is the aarch64/riscv64 shell: D10, measured) gained an exit-status spine (`process_command` returns a status; `cmd_run_argv` returns the child's exit code instead of discarding it) and `sh <file> [args]` with `$0..$9`/`$#`/`$?`, a 4-deep frame stack, line-numbered failures (`sh: /tests/sh6a_fail.sh:8: command failed with status 127`) and an `exit N` that stops the script rather than halting PID 1. The pure expander lives in `userspace/system/init/sh_expand.h` so the host tests the shipped body (`tests/unit/test_sh_expand.c`, 64 assertions, mutation-checked); `tests/integration/cases/test_selfhost_script.sh` (**12/12**, selfhost shard, needs no guest toolchain so it never skips) drives four in-guest scripts staged at `/tests`. Receipt `[selfhost] script PASS: 7 lines ran in-guest`. **SH6b landed** — `>`, `>>`, `<`, `set NAME=VALUE`/`$NAME`, and the quote-aware tokenizer they depend on (new pure `userspace/system/init/sh_parse.h`, 163 lines; quotes and redirects recognised in one pass, because whether `>` is an operator depends on whether it is quoted). Expansion runs per token, so a variable's value cannot inject an argument or an operator. This needed a **kernel fix the SH6 survey wrongly ruled out**: `SYS_WRITE` sent fd 1/2 to the console unless the slot held a pipe and `SYS_READ` sent fd 0 to the keyboard, so a redirect created and truncated the file and then wrote to the console anyway (measured: 0 bytes). `vfs_fd_is_pipe` became `vfs_fd_is_devfs` — fd 0/1/2 keep the console path only while they refer to a devfs node, so regular files and pipes are honoured and `gterm` is unchanged. `tests/unit/test_sh_parse.c` 87 + `test_sh_expand.c` 122 assertions against the shipped headers, mutation-checked; `tests/integration/cases/test_selfhost_redirect.sh` **15/15** (its load-bearing assertion is `cat < a > b`, both directions on one line). Receipt `[selfhost] redirect PASS: 2 files written and read back`. **SH6c landed** — `|` on the existing `SYS_PIPE`, and `;`/`&&`/`||` consuming the SH6a status spine. Stages of a pipeline run sequentially in the shell (each stdout wired to the next stdin through a real kernel pipe); pipeline status is the last stage's (POSIX). Per-stage fork was measured to `#PF` (error 0x6) on child resume (ledger SH-40) and is not this sub-phase's kernel work. `tests/unit/test_sh_parse.c` covers the new operators; `tests/integration/cases/test_selfhost_pipe.sh` is the gate. Receipt `[selfhost] pipe PASS: 4 pipelines ran`. **SH6d landed** — `if`/`elif`/`else`/`fi`, `while`/`do`/`done`, `for x in <words>`, `break`. Keywords stay words; compounds collect from a line-source so a nested `if` inside a `while` does not swallow `done` (ledger SH-41). `true`/`false`/`break` are builtins. `tests/integration/cases/test_selfhost_control.sh` is the gate. Receipt `[selfhost] control PASS: 5 branches and loops ran`. **SH6e landed** — `tools/shmake/shmake.c` / `/bin/shmake`: rules, `$(CC)`, `.PHONY`, timestamps.  Recipes exec via `spawnv` (no `/bin/sh`, ledger SH-42).  `test_selfhost_shmake.sh` is the gate.  Receipt `[selfhost] shmake PASS: 3 targets up to date`. **SH6f landed (SH6 complete)** — `sh build.sh kernel` drives `Selfhost.mk` on `/fat` (D5 target set `kernel initrd iso user`, D6 resume: stop at phase 6 of 9, reboot, reprint the receipt without rebuilding).  `test_selfhost_build.sh` is the gate.  Receipt `[selfhost] build PASS: kernel+initrd built on /fat`. **SH7 split into SH7a–SH7e (the C image twins), SH7a landed** — `/bin/sha256sum` (`tools/selfhost/sha256sum.c`) is the first in-guest image tooling twin: a coreutils-shaped hash program that reuses the single `libatls` SHA-256 (no second implementation), with `--selftest` (FIPS 180-4 / RFC 6234 vectors) and `--eq FILE` (stdin↔file digest parity as an exit status, since the scripting shell has no `cut`/`grep` to parse a digest line); host gate `tests/unit/test_sha256sum.c` (10 checks), guest gate `tests/integration/cases/test_selfhost_sha256sum.sh` (7/7, selfhost shard) via `tools/selfhost/sh7a_probe.sh`; receipt `[selfhost] sha256 PASS: selftest + stdin + file parity verified in-guest`. **SH7b landed** — `/bin/mkinitrd` (`tools/selfhost/mkinitrd.c`) is the USTAR writer twin: freestanding C, packs a tree into the exact format `kernel/fs/initrd.c` reads (`ustar\0` magic, typeflags, octal fields, checksum, `"./"` names), with `--list` and `--selftest` modes so the shell branches on `$?`; host gate `tests/unit/test_mkinitrd.c` (GNU tar lists/extracts it, every member round-trips), guest gate `test_selfhost_mkinitrd.sh`; receipt `[selfhost] mkinitrd PASS: <n> members written in-guest`. **SH7c landed** — `/bin/bootoffsets` (`tools/selfhost/bootoffsets.c`) derives `boot_info_t` offsets with `offsetof()` from the same `boot/shared/boot_info.h` as the host generator (`--c`/`--asm`/`--check`); host gate `tests/unit/test_bootoffsets_twin.c` (12 offsets + sizeof parity), guest gate `test_selfhost_bootoffsets.sh`; receipt `[selfhost] boot-offset header PASS: generated in-guest`. **SH7d landed** — `/bin/mkiso` (`tools/selfhost/mkiso.c`) writes the whole hybrid disk: dual MBR (spliced `mbr_dual.bin`, FAT32-LBA + GPT-protective slots), Stage 2 at LBA 34, primary+backup GPT (header/array CRC32, ESP type GUID), and a native FAT32 ESP (two FATs, FSInfo+backup, FAT[0]=`0x0FFFFFF8`, root cluster 2, 8.3 tree, contiguous clusters) — replacing mformat/mcopy and the inline BPB-patching python3 (the ≥65525-cluster FAT32 floor is enforced; `make iso` now uses this same C source host-side, so mtools/mcopy are gone from the ISO path). The C-written image boots BOTH firmware paths in QEMU (OVMF→`booted via UEFI`, SeaBIOS Stage2→`booted via BIOS`, initrd parsed 123 files); host gate `tests/unit/test_mkiso.c` parses MBR/GPT/BPB/FSInfo/FAT/8.3 and cluster-chain round-trips plus the sub-floor rejection, guest gate `test_selfhost_mkiso.sh`; receipt `[selfhost] mkiso PASS: <image> written in-guest`. SH7e (build+boot the guest ISO end-to-end) and SH8–SH9 pending. |
| `fork` | 🧪 | COW fork via `paging_clone_user_space()` (O(page-tables) not O(address-space)); simplified PID semantics. |
| `execve` | 🧪 | Replaces current address space, simplified. |
| `wait4` / `wait` | 🧪 | Yield-polling, no precise child PID semantics. |
| SA_RESTART syscall restarting | 🧪 | Restartable blocking syscalls save restart metadata on `-EINTR` and are transparently re-dispatched from `sigreturn` when the handler was installed with `SA_RESTART`; signal frames preserve FPU/SSE state via `FXSAVE`/`FXRSTOR`. |
| Thread/process reaping | ✅ | Dead TCBs/stacks are deferred-reaped from a safe stack via `thread_reap_zombies()` called every PIT tick. User address spaces are fully freed by `paging_free_address_space()` (walk user PML4, free all data frames + page-table frames + PML4 itself). COW shared frames use PMM refcount — frame is only returned to the free pool when all sharers have released it. Boot log confirms: `[thread] reaped '/hello' (tid 6, 35 frames)`. |
| Per-process FD tables | 🧪 | Each TCB has its own FD table; `fork` shallow-copies entries. Lifetime/inheritance semantics remain simplified. |

## Filesystems

| Feature | Status | Notes |
|---|---:|---|
| VFS mount table | ✅ | Longest-prefix mount matching. |
| `readdir` / `mkdir` / `unlink` / `rename` / `stat` | ✅ | Generic VFS ops + matching syscalls (`100..105`); `stat()` exposes size/mode/link metadata plus seconds-resolution `mtime`/`ctime`/`atime` where supported. Baseline `mkfifo` named FIFOs, `symlink`/`readlink`, and `lstat`/`fstat` are wired (in-memory, `/tmp`-tested). |
| USTAR initrd | ✅ | Read-only root with user ELFs. |
| DevFS | ✅ | `/dev/null`, `/dev/zero`. |
| `tmpfs` | ✅ | Writable in-memory `/tmp`, supports `unlink`, `truncate`, and seconds-resolution `stat()` timestamps. |
| Tiny diskfs | ✅ | Persistent `/disk` (8 files × 4 KiB, AHCI port 0) with persistent stat timestamps in the on-disk table. |
| **FAT32 — full** | ✅ | `/fat`: subdirs, **LFN (UCS-2 read+write)**, mkdir/rmdir/unlink/rename/truncate, FSInfo, FAT date/time stamps decoded through `stat()`. |
| **ext2 — full** | ✅ | `/ext2`: mounts existing Linux-mkfs images **and** formats blank disks in-kernel. Direct + single/double/triple indirect blocks; mkdir/rmdir/unlink/rename; inode timestamps; cross-OS round-trip verified with `debugfs`. |
| buffer cache | 🧪 | Buffer cache layer for block I/O caching and synchronization. |
| exFAT | 🚧 | skeleton |
| NTFS | 🚧 | skeleton |
| ext4 | 🚧 | experimental ext4-like |
| Btrfs | 🚧 | experimental CoW prototype |
| F2FS | 🚧 | experimental log-structured prototype |

## Syscalls

| Area | Status | Notes |
|---|---:|---|
| Console/file I/O | ✅ | `read`, `write`, `open`, `close`. |
| Process basics | 🧪 | `getpid`, `exit`, `spawn`, `fork`, `execve`, `wait4`. |
| Directory/path ops | ✅/🧪 | `listdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `truncate`, `stat`. |
| Networking | 🧪 | DNS with cache+failover+CNAME (X3), ping, legacy TCP calls and process-owned socket-style syscalls; IPv4 fragment reassembly (X4) feeds oversized UDP/TCP datagrams to the normal path. |
| GUI | ✅/🧪 | `SYS_GUI_CALL` (200), `SYS_GUI_EVENT` (201), `SYS_GUI_THEME` (202). v2.0 theme engine, icons, notifications, snap, context menus. |
| Memory syscalls | 🧪 | `brk` implemented. `mmap`/`munmap` support eager private anonymous mappings and eager private file-backed reads. |
| Sockets | 🧪 | AF_INET/SOCK_STREAM handles include `socket`, `connect`, `send`, `recv`, `close`, plus server-side `bind`, `listen`, and `accept` (`305..307`). AF_INET/SOCK_DGRAM supports `sendto=44` and `recvfrom=45`. |
| Entropy (`getentropy`/`getrandom`) | ✅ | INTERNET_PLAN N0. ChaCha20 CSPRNG (`kernel/rng_core.h`, RFC 8439) seeded from RDSEED/RDRAND when present, else an interrupt-timing jitter pool stirred on every IRQ. Fails closed: `getentropy` returns `-ENOSYS` and `getrandom` blocks (or `EAGAIN` with `GRND_NONBLOCK`) until real entropy exists; estimated entropy logged at boot. Host RFC-vector + 1 MiB statistics tests, QEMU gate `test_rng.sh`. |
| Crypto primitives (`libatls`) | ✅ | INTERNET_PLAN N1/N2. Userspace static library `lib/libatls/`: SHA-256/512, HMAC-SHA256, HKDF, ChaCha20/Poly1305 AEAD, X25519, Ed25519 verify — all RFC-vector-verified by `tests/unit/test_atls_*` (155 checks) incl. the X25519 1000-iteration run and ten Wycheproof low-order triples. N2 adds zero-copy depth-bounded X.509 v3 parsing (`atls/x509.h`, `ATLS_DER_MAX_DEPTH` 32, iterative skipper): real-leaf field batteries, 10 000-deep refused, mutation corpus — the hostile half re-run in-guest by `/tests/x509test` on the 64 KiB stack. D7 enforced by a source grep: no `memcmp` on secrets, only `atls_ct_eq`. Shipped in the SDK (`AURALITE_LIBS_TLS`); gates `test_crypto.sh`, `test_x509.sh`.  **Y5 landed:** ML-KEM-768 (FIPS 203) + SHA3/SHAKE (`test_atls_mlkem` 23/23, ACVP sample + FO implicit rejection); EXECUTED at x86_64/`-m32`/rv64/a64.  **Y6 landed:** ClientHello offers `X25519MLKEM768` (0x11EC) + X25519; host `test_atls_tls` 32/32; guest `[tls] PASS: X25519MLKEM768`.  **Y7 closed the series:** `docs/live_web.md` is the pending-user paste-back; libahttp prints `[tls] group=`. |

## Networking

| Feature | Status | Notes |
|---|---:|---|
| **Trust-store lifecycle (X8)** | ✅ | `docs/trust_store.md` records the decision (b: documented rebuild-and-reship with a dated provenance file), lists the shipped roots with SHA-256 fingerprints, not-before/not-after and source, and records that OCSP/CRL/CT are excluded. `ATLS_CERTVAL_ERR_UNKNOWN_ROOT` (-27) is a distinct "root not in trust store" diagnosis (surfaced by `libahttp`, not a generic handshake failure). `trustinfo` (`/apps/trustinfo`) prints each shipped root's common name and not-after expiry in the guest. |
| **Fit + honest statement (X9/N9)** | ✅ | Measured: the largest browser binary (`gbrowser`) is 380,904 bytes = 2.3% of the 16 MiB `SPAWN_MAX_IMAGE` (SELFHOST SH1: was 1 MiB); no initrd binary is over the limit, and the user stack is 4 MiB (was 1 MiB) with ample TLS headroom. `tls.md` §6.5 is the honest security statement (not audited, no side-channel review beyond D7, OCSP/CRL/CT excluded, not for protecting anything valuable); `WEBVIEW_PLAN.md` D6 updated (HTTPS is implemented, not out of scope); stale "64 KiB"/"HTTPS is not supported" claims removed. `sysinfo` prints the limit and stack numbers. |
| NIC abstraction (netdev) | ✅ | `kernel/net/netdev.{h,c}` selects an active NIC backend at boot; the IPv4/ARP/DHCP/UDP/TCP stack talks to it via `netdev_*` instead of a specific driver. Priority order is e1000 → virtio-net → rtl8139; the first backend to register becomes active. |
| **IPv6 (X7+R9)** | 🧪 | X7's landing plus R9: SLAAC (RA Prefix Information, A-flag, /64 + EUI-64 — `fec0::…` under SLIRP), default-router learning, an NS→NA responder (the missing NDP half), off-link routing via the RA'd router, and source selection (global src for global dst).  Deterministic gates: self-ping AND `ping6 fec0::2` answered end-to-end in CI — the "SLIRP filter (Launchpad #1724590)" X7 recorded turned out to be FIVE of our own bugs (RS/NS length +4 with a checksum over garbage tail bytes, wire checksums stored byte-swapped, NA target read at +12 instead of +8, RA options parsed from +8 instead of +16, and a validator that could never pass), all pcap-named in R9.  **Y3 landed:** AF_INET6 sockets, TCP-over-IPv6 (`tcp_open_addr` / `netl3_v6_ops`), AAAA parse+query, dual-stack pick.  **Y4 landed:** HTTPS-over-IPv6 via libahttp (`[https6] PASS`); RES-26 DONE@Y4. |
| PCI e1000 detection | ✅ | Supports common QEMU/VirtualBox/VMware 8254x IDs. |
| e1000 TX/RX | 🧪 | Legacy descriptor rings with INTx IRQ enable, IRQ cause handling, preallocated software RX queue, non-blocking compatibility receive, and blocking/timed receive helpers. TCP, ARP, DHCP, ICMP, and kernel UDP/DNS receive waits now use IRQ/wait-queue-backed bounded NIC waits; user UDP sockets (`sendto`/`recvfrom`) and basic fixed-RTO TCP retransmission are implemented; remaining N2 work is deeper socket blocking edge cases and production TCP features. |
| Ethernet / ARP | ✅ | Gateway routing support. |
| IPv4 / ICMP | ✅ | Ping self-test; bounded RFC 1122 fragment reassembly (8 datagrams × 8 KiB, 10 s timeout, first-win overlap, LRU eviction) wired into every RX path — UDP, ICMP and both TCP receive loops step through `net_ipfrag_step()` and parse completed datagrams as ordinary packets. REALINTERNET_PLAN X4; gated by `test_ip_reasm` (host, 11 scenarios) and the in-kernel wire self-test in `test_ip_frag.sh` (guest, 6 asserts). |
| DHCP | ✅ | QEMU/VM NAT-oriented DORA flow. |
| UDP | ✅ | Used by DNS and exposed to userspace through AF_INET/SOCK_DGRAM `sendto`/`recvfrom`. |
| DNS resolver | ✅ | A-record lookup with a TTL/negative cache (LRU, RFC 2308 SOA-derived negative TTL), up to 4 servers from DHCP option 6 with visible timeout failover, CNAME-chain chasing (in-packet and re-query), strict wire validation (ID/QR/compression bounds), LRU expiry re-query; `SYS_DNSCTL` + shell `dnscache`/`dnsset`/`dnsflush`. REALINTERNET_PLAN X3; gated by `test_dns` (host, 14 scenarios) and `test_dns_cache.sh` (guest, 10 asserts). |
| TCP client | 🧪 | Per-connection state for up to 16 concurrent connections (raised 8→16 in X5; the ~64 KiB dead buffer per handle was removed, so RAM cost went *down*). Sliding send window (cwnd + peer window), RFC 6298-style adaptive RTO with exponential backoff (1 s initial, 200 ms min, 60 s cap, Karn's rule), PMTUD black-hole segment-size ladder (1460→536 on unbroken timeouts), visible give-up (-ETIMEDOUT) after 10 unbroken RTOs, RST handling (-ECONNRESET) in send/recv/close, and inbound sequencing — in-order / duplicate / partial-duplicate / single-gap out-of-order stash and chain. Fixed ISNs per handle are a documented simplification. REALINTERNET_PLAN X5; gated by `test_tcp_x5` (host, 8 scenarios) and `test_tcp_x5.sh` (guest, 9 asserts: 16-connection boot gate + 1 MiB piecemeal-ACK upload). Legacy `SYS_NET_*` are deprecated. **RINET2 Y2:** the transport talks to L3 only through `netl3` (family + 16-byte key, v4 ops); tcp.c has zero inline IPv4 spellings. **Y3:** TCP-over-IPv6 via the same seam (`tcp_open_addr`, `netl3_v6_ops`); `AF_INET6`/`sockaddr_in6`/`connectaddr`; AAAA parse+query; dual-stack pick prefers v6 when a global address exists. Guest receipt `[tcp6] PASS`. |
| Socket API | 🧪 | AF_INET/SOCK_STREAM process-owned handles plus AF_INET/SOCK_DGRAM `sendto`/`recvfrom` exist. |
| TCP server (bind/listen/accept) | 🧪 | Server-side socket path is implemented with `tcp_listen()`/`tcp_accept()` and the `/tcpserver` minimal HTTP echo server. |
| virtio-net | 🧪 | Modern virtio PCI driver (`drivers/virtio_net/`) with a real data path: `VIRTIO_F_VERSION_1` negotiation, RX (queue 0) / TX (queue 1) split virtqueues, MAC read from device config, and a 12-byte `virtio_net_hdr` (no `MRG_RXBUF`). Registers as a netdev backend; selected when e1000 is absent. Validated end-to-end (DHCP + ICMP + DNS + TCP) under QEMU `-device virtio-net-pci`.  R9 (RES-28): the receive path now SLEEPS on the wait queue the RX interrupt wakes (`wq_wait_deadline`) instead of pause-spinning — receipt `[virtio-net] RX via IRQ wake` pinned in the case.  (The ISR + wake existed before R9; nothing ever slept, so the interrupt was decorative — a half-stale ledger row, named.) |
| RTL8139 family | ✅ | Realtek 8139 driver (`drivers/rtl8139/`) with a real data path: port-I/O register file (BAR0), an 8 KiB RX **ring buffer** walked through CAPR/CBR, four hardware TX descriptors round-robin, INTx-driven RX that wakes sleepers (receipt `[rtl8139] RX via IRQ wake`), runt padding to 60 bytes, and an explicit refusal when a DMA buffer lands above the 4 GiB the 32-bit `RBSTART`/`TSAD` registers can express. Accepts `10ec:8139/8138/8100/8130`. Validated end-to-end under QEMU `-device rtl8139` (DHCP + ICMP + DNS + 16 concurrent TCP): `tests/integration/cases/test_rtl8139.sh`, 18/18. The ring arithmetic — FCS stripping, wrap, the CAPR −16 bias — is a pure-C core (`rtl8139_ring.h`) with a 203-check host gate, which is where the ring-modulus bug (allocation vs. `RCR.RBLEN` ring proper; killed RX permanently after ~128 packets) is now pinned. |
| vmxnet3 / e1000e / RTL8169 | 🚧 | Recognised by the virtual-driver probe, but no data path yet. RTL8169/8168 is a different chip from the 8139 (descriptor rings, not a ring buffer) and QEMU does not emulate it, so it could not be gated. Use e1000, virtio-net or rtl8139. |

## Graphics and input

| Feature | Status | Notes |
|---|---:|---|
| Framebuffer console | ✅ | Linear framebuffer handed over in `boot_info_t`. Double-buffered; note that `gfx_fill_rect()` omits the `back_fb` NULL guard its siblings have (`TODO.md`). |
| PSF/bitmap font rendering | ✅ | Embedded console font. |
| 2D graphics | ✅ | Double-buffered drawing. |
| Window manager demo | ✅ | Windows, widgets, taskbar, mouse interaction. |
| PS/2 keyboard | ✅ | Scan-code set 1, ASCII + rich key-event queues. **US layout only** — two fixed translation tables, no keymap selection, no dead keys. |
| PS/2 mouse | ✅ | IRQ 12, cursor/buttons and wheel-event support. |
| Kernel GUI/compositor | ✅ | **v2.0**: Theme engine (30+ params), desktop icons (32), notifications, window snapping (left/right/top/bottom/maximize), start menu, context menus, always-on-top windows, tool windows, edge/corner resize, double-click titlebar maximize, alpha blit, explicit event ABI (#define values), 64 windows, 128-event rings. Dirty-rect partial redraw is implemented via `compositor_render_dirty()` and `gfx_flip_rect()`, with idle frames skipping flips. Per-process window/icon cleanup on exit. |
| 3D software renderer | 🧪 | Demo renderer, CPU/SSE float math. |
| **OpenGL (libgl)** | 🚧 | User-space GL 1.1/1.3 stack with FBOs and an ES 2.0 shader path, software rasterizer — see `GL_PLAN.md`. **G0-G9 complete**: hardened `GUI_OP_BLIT` presentation path, libc float math, `glmath` layer, AuraGLX context, matrix stacks + immediate mode (all ten primitive modes), and a **filled edge-function rasterizer** with depth buffer (all 8 compare functions, `glDepthMask`), back-face culling, top-left fill rule, scissor test and `glPolygonMode`. 168 host unit tests + 38 QEMU assertions. `/glcube` renders a **solid depth-buffered cube**. Plus **frustum clipping** against all six planes (Sutherland–Hodgman for triangles, Liang–Barsky for lines, attributes interpolated at the cut) and `glPushAttrib`/`glPopAttrib`. 196 host unit tests + 46 QEMU assertions. The camera can fly through geometry without artefacts. Plus the full **GL 1.1 lighting model**: 8 lights (positional/directional/spot), Blinn–Phong specular, distance attenuation, front/back materials, `GL_COLOR_MATERIAL`, inverse-transpose normal matrix and `GL_NORMALIZE`. 228 host unit tests + 53 QEMU assertions. `/glcube` renders a lit, depth-buffered cube. Plus **texturing** (2D texture objects, all five base formats, nearest/bilinear filtering, repeat/clamp wrapping, `GL_MODULATE`/`REPLACE`/`DECAL`/`BLEND`, **perspective-correct** UV interpolation), **blending**, the **alpha test** and **fog** (linear/exp/exp2). 265 host unit tests + 63 QEMU assertions. `/glcube` renders a lit, textured, depth-buffered cube. Plus **vertex arrays** (all four attribute arrays, arbitrary stride, eight component types), **buffer objects** (GL 1.5 subset, both targets) and **display lists** (command-log based, so matrix and state commands replay correctly). 301 host unit tests + 71 QEMU assertions. `/glcube` compiles its cube into a display list and draws the grid from a vertex array. Note: arrays are an API-completeness feature, not a speed-up — the per-vertex transform dominates, so `glDrawArrays` measures the same as immediate mode. Plus the **GLU layer** (`gluPerspective`, `gluLookAt`, `gluOrtho2D`, `gluErrorString`, sphere/cylinder/disk quadrics) and two shipped demos: `/glcube` and `/glgears` — the latter ported from real OpenGL sources with the GL calls unchanged, which is the strongest available evidence the API behaves as applications expect. 322 host unit tests + 80 QEMU assertions. See [`docs/opengl.md`](opengl.md). Plus the **backend seam** (`gl_backend_t`, modelled on `netdev`): the software rasterizer is registered as an ordinary backend, any entry point may be NULL with software covering the rest, and `glGetString(GL_RENDERER)` reports the active one. A VirGL candidate is registered but declines until the kernel exposes a user-space 3D submission syscall. Plus **GL 1.2/1.3 texturing** (G10): mipmap chains with all four mipmap filters and `glGenerateMipmap`/`gluBuild2DMipmaps`, **two texture units** (`glActiveTexture`, `glClientActiveTexture`, `glMultiTexCoord2f`, per-unit environment, combined in fragment order), **3D textures** with trilinear sampling, **cube maps** with major-axis face selection, and `GL_CLAMP_TO_BORDER`. The mipmap level is chosen **per triangle** rather than per fragment — a scanline rasterizer has no `dFdx`/`dFdy`, so the level comes from the texture-space/screen-space area ratio; large receding surfaces must be tessellated, which `/glcube`'s new mipmapped floor demonstrates. Measured: `GL_NEAREST_MIPMAP_NEAREST` is *faster* than un-mipmapped `GL_LINEAR` (2.6 vs 3.2 ms/frame), trilinear costs 1.9×. This phase also uncovered and fixed a latent G1 bug — `aglxResize()` held a whole scratch context on the stack, which page-faulted once the context passed 130 KB. Plus **framebuffer objects and render-to-texture** (G12): `glGenFramebuffers`/`glBindFramebuffer`/`glFramebufferTexture2D`/`glFramebufferRenderbuffer`/`glCheckFramebufferStatus`, colour and depth renderbuffers, and `glReadPixels` in six formats reading whichever target is bound. The rasterizer needed **no changes at all** — an FBO re-points the four fields (`color`, `depth`, `width`, `height`) it has always written through, which is the return on having kept the render target abstract since G3. Rendering into an FBO measures the same as rendering into the window (3.72 vs 3.75 ms/frame); only the bind/unbind pair costs anything, and that scales with attachment area because unbinding forces the rendered texture opaque. Two real bugs surfaced and were fixed: `gl_fb_row()` flipped y unconditionally, correct for the window and upside-down for a texture; and a rendered texture sampled as fully transparent because the rasterizer writes no alpha byte. `/glcube` gained an inset render-to-texture panel showing a second view of the scene. Plus the **GLSL ES 1.0 front end** (G11a, the first of four shader sub-phases): a lexer, recursive-descent parser and type checker producing a typed AST, in 2400 lines. The full language — scalar/vector/matrix/sampler types, structs, arrays, functions with `in`/`out`/`inout`, the built-in library — with stage-aware rules (`attribute` refused in a fragment shader, `discard` in a vertex shader, a diagnostic when a vertex shader never writes `gl_Position`). Diagnostics are treated as the deliverable rather than an afterthought: every one carries a line number and names the rule, and most of the 167 host tests are negative cases asserting on the message text. One arena per compilation means a failed parse leaks nothing; every limit on untrusted input is a diagnostic rather than a fault. Not yet reachable from the GL API — `glCreateShader` arrives in G11c. Measured: 0.037 ms and 140 KB to compile a 22-line Blinn-Phong shader. Plus the **GLSL execution engine** (G11b): an AST-walking interpreter, 1500 lines, running the whole language — expressions, control flow, user functions with `in`/`out`/`inout`, structs, arrays, swizzled lvalues, matrix algebra and the built-in library. A shader reaches the outside world through three `glsl_env_t` callbacks, so the engine is testable with no GL context and G11c can attach the real pipeline without changing it. Semantics that differ from C are tested explicitly: `mod()` takes the sign of the divisor, integer division truncates towards zero, `matN(s)` is a diagonal, and undefined maths yields finite values rather than NaNs that would propagate through blending. Every limit on untrusted input (100k iterations, call depth 16, argument nesting 24) is a diagnostic rather than a fault. Three bugs were found only by running on the target, where the user stack is 64 KB against the host's 8 MB: two scratch arrays living on the C stack (5.9 KB and 1.2 KB per frame) and a shared argument buffer keyed on call depth rather than argument nesting, which made `max(dot(a,b), 0.0)` silently ignore its clamp. A kernel limit surfaced too: `spawn()` truncated executables over 256 KB without saying so. Measured 0.27 µs per trivial fragment shader invocation — 20 ms/frame at 320×240, against 0.07 ms for the entire fixed-function path, exactly the one-to-two orders of magnitude the plan predicted. The shader path buys API coverage, not frames per second. Plus the **shader pipeline** (G11c): shaders are reachable from the GL API and draw pixels — `glCreateShader` through `glUseProgram`, generic vertex attributes, uniforms including the matrix forms. The vertex shader replaces the transform, the fragment shader replaces texturing/lighting/fog, and varyings ride through the existing clipper and perspective-correct interpolator; clipping, culling, depth, scissor and blending are untouched because a shader changes neither the window coordinates nor the meaning of a colour. Linking builds the uniform/varying/attribute tables once so the interpreter's by-name lookups become index arithmetic, and a varying the fragment shader reads but the vertex shader never declares is a **link error** rather than a silent read of zeros. Assembling the pipeline exposed a 30× interpreter regression invisible in G11b's standalone benchmark: `glsl_run()` allocated and zeroed its 90 KB state on every invocation, which at one call per pixel cost 3.90 µs per fragment against 0.27 µs measured directly — caching it took a full-screen shaded frame from 306 ms to 12 ms. Measured at 320×240: fixed function 0.92 ms/frame, constant shader 12.1 ms, Lambert-lit 53.8 ms, vertex stage alone 1.2 µs/draw. Vertex shaders are affordable; full-screen fragment shaders are not. Plus **coexistence hardening** (G11d), the sub-phase the plan flagged as carrying most of the risk — correctly, since audit found four real defects. Shaded points and lines were **not shaded**: they wrote the vertex colour, which the shader path leaves at white, so a shaded `GL_LINE_LOOP` came out white; every G11c test drew triangles, so nothing exercised those rasterizers with a program bound. Immediate mode silently hybridised — the fixed-function matrices placed the geometry and the fragment shader coloured it, a combination no GL implementation produces and one that would render plausibly here and draw nothing on real hardware; now `GL_INVALID_OPERATION`, with the draw calls exempt. `glUseProgram` was accepted inside `glBegin`/`glEnd` and executed immediately inside a display list, the latter silently rebinding the current program as a side effect of compiling. Verified rather than assumed: scissor, culling, depth mask, blending and FBO rendering all apply to shaded fragments, while lighting, fog, the alpha test, the texture environment, `glShadeModel`, the fixed-function matrices and arrays all correctly fail to reach one. The audit was a probe program enumerating interactions rather than tests written from the specification — three of the four defects were combinations nobody would have thought to assert on. Plus the **VirGL hardware backend** (G13): `libgl/src/glvirgl.c` reaches a real virtio-gpu through `SYS_GPU_CALL` — probe, clear, and present via TRANSFER + SET_SCANOUT + RESOURCE_FLUSH. `DRAW_VBO` is deliberately absent: it needs shaders as TGSI, and G11's compiler produces an interpreted AST, so a TGSI back end is a compiler phase rather than a corner of this one. The phase also closed the K1 blocker — `op_transfer()` shipped copying its payload into a bounce buffer and freeing it **unused**, because a `RESOURCE_CREATE_3D` resource has no guest memory behind it; resources now get backing, released on destroy and on process teardown, the latter having been a permanent physical-memory leak. Three wire-format constants were wrong in the first draft and all were caught before reaching a device (two by a unit test running the kernel's own validator); the file now includes `drivers/gpu/virgl.h` rather than restating it, so there is one definition of the protocol in the tree. Attaching a real GPU also surfaced a **pre-existing driver hang** during virtio-gpu initialisation — bisected to before G11d, never exercised because no integration case attached a GPU until G13 added one; recorded in `TODO.md` with what has been ruled out. Honest framing: this buys a proved seam, not frames per second. **GL_PLAN.md G0–G13 complete** — the plan is finished: 967 host unit checks + 373 in-OS checks + 86 QEMU assertions. |
| GUI bulk pixel blit | ✅ | `GUI_OP_BLIT` / `GUI_OP_BLIT_ALPHA` + `ag_blit()`/`ag_blit_alpha()`. Source rect is fully validated with `validate_user_range()`, then copied row-by-row through a kernel bounce buffer, so no raw user pointer is ever dereferenced. Covered by `test_opengl.sh`. |
| Native VBox/VMware SVGA drivers | ❌ | The bootloader-provided framebuffer is used instead. |
| virtio-gpu 2D scanout | 🧪 | Modern virtio-gpu PCI probe, control queue, RESOURCE_CREATE_2D, ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D and RESOURCE_FLUSH are implemented as an optional mirror path for `gfx_flip()`. The driver is initialised during graphics boot so mirroring is available before the GUI compositor starts. |
| GPU 3D submission syscall | 🧪 | `SYS_GPU_CALL` (203) exposes the kernel VirGL transport to user space: per-process resource handles, command-stream validation on a kernel-side copy, quotas (4 contexts / 64 resources / 64 MB), scanout restricted to PID ≤ 2, and reaping on process exit. 18 host unit checks against malformed streams. The final `transfer` hop still needs a driver entry point taking fresh data; see `GL_PLAN.md` phase K1. |
| virtio-gpu VirGL command transport | 🧪 | Feature negotiation, 3D context create/destroy, RESOURCE_CREATE_3D, context attach/detach, fenced SUBMIT_3D payload chains and TRANSFER_TO_HOST_3D are present. A tiny VirGL command-stream builder submits clear/framebuffer plus experimental vertex-buffer/triangle packet streams, and a full present pipeline now drives TRANSFER_TO_HOST_3D + SET_SCANOUT + RESOURCE_FLUSH to scan a 3D render target out to the display (with graceful software fallback when no virgl host is attached). Command-stream encoding is host-unit-tested (`test_virgl`). There is still no full OpenGL/Gallium state tracker. |

## Storage and USB

| Feature | Status | Notes |
|---|---:|---|
| POSIX.1-2024 compliance | 🧪 | Q1–Q16 all implemented (~410 functions covered, see `docs/posix2024_compliance.md`). Q12 added the runnable conformance harness (host gate in `make test-unit` — `tests/posix2024/` — plus the guest `conformtest` QEMU case); Q13 completed the AT-family; Q15 implemented `mq_notify`; Q16 added pselect/ppoll, getrandom and sig2str/str2sig (getrandom's backing was upgraded by INTERNET_PLAN N0 to a ChaCha20 CSPRNG seeded from RDSEED/RDRAND or interrupt jitter, failing closed with ENOSYS until real entropy exists); Q14 replaced the SysV IPC ENOSYS stubs with real kernel objects — semaphores (blocking semop, SEM_UNDO at exit), page-backed shared memory (destroy at last detach) and message queues (full mtype rules). Only the three named-semaphore functions remain argued 🔶 partials (need MAP_SHARED). `_POSIX_VERSION` = 202405L. |
| AHCI detection/init | ✅/🧪 | Controller/port setup works in QEMU AHCI. |
| AHCI sector read/write | ✅/🧪 | DMA READ/WRITE self-test passes on the QEMU AHCI test disk. |
| UHCI controller | ✅/🧪 | Controller + port + CONTROL/BULK TD/QH transfers used by MSC. |
| OHCI controller | 🧪 | Detection/init/root-port reset plus ED/TD control, bulk and interrupt scheduling works in QEMU for HID/MSC. |
| EHCI controller | 🧪 | Detection/init/root-port reset plus async qTD control/bulk scheduling works in QEMU for high-speed MSC. Periodic interrupt and split transactions are pending. |
| xHCI controller | 🧪 | **Real.** Event ring and command ring (U1), Enable Slot / Address Device with `Slot State` read back from the device context (U3), control transfers with short-packet residue and stall recovery (U4), bulk (U5), interrupt endpoints (U6), nested hubs with correct route strings (U9). The interrupt is taken and acknowledged, but the event ring is still drained by the polling consumer, so hotplug latency is the poll's (~400 ms), not interrupt time — see [`docs/usb.md`](usb.md) and [`USB_PLAN.md`](../USB_PLAN.md) U8. Streams/UAS absent. |
| USB device enumeration | 🧪 | Real on all four controllers. On xHCI descriptors are fetched from the device: distinct VID/PID and product strings per device, configuration descriptors parsed in two passes, endpoints decoded with real addresses and packet sizes. |
| USB HID keyboard/mouse | 🧪 | Works on UHCI, OHCI, EHCI and xHCI. `test_usb_hid_input.sh` asserts the HID usage codes of injected keystrokes and keeps a no-USB control run, because QEMU's `sendkey` also drives the emulated PS/2 keyboard and would otherwise make the test pass without USB doing anything. EHCI interrupt endpoints are on the periodic schedule (U7); TT split transactions are implemented but untestable in QEMU. |
| USB hubs | 🧪 | Hub descriptor/status, port power/reset and downstream enumeration, including devices two hubs deep with correct xHCI route strings (`test_usb_hub_depth.sh`). Depth limit 5, matching USB. |
| USB hotplug monitor | 🧪 | Attach/detach detected and drivers bound; `test_usb_hotplug.sh` passes 5/5. The xHCI IRQ is registered and taken, but port-change events do not arrive through it under QEMU, so the 10 ms/500 ms poll is still doing the work (U8, partial). Slots are freed on detach. |
| USB Mass Storage | 🧪 | Bulk-Only/SCSI on UHCI, OHCI, EHCI and xHCI. `test_xhci_bulk.sh` writes a known pattern into sector 0 with `dd` and requires those exact bytes back, so the read is verified against the disk rather than against the driver. Media exposed at `/usb` via usbfs, FAT32 read-only under `/usb/fat`. |

## Wireless and Bluetooth

| Feature | Status | Notes |
|---|---:|---|
| Bluetooth HCI protocol | 🚧 | HCI commands/events implemented; depends on USB transport. |
| Wi-Fi 802.11 MAC layer | 🚧 | Management frames/state machine; no chipset driver registered by default. |

## Userspace applications

| App | Status | Notes |
|---|---:|---|
| `/init` shell | ✅ | Interactive serial/keyboard shell; 37 builtins, job control (`jobs`/`fg`/`bg`), exit statuses and script execution (`sh <file> [args]` with `$0..$9`/`$#`/`$?`) since SELFHOST SH6a; redirects (`>`/`>>`/`<`), shell variables (`set`/`unset`/`$NAME`) and quote-aware parsing since SH6b; pipes (`|`) and command lists (`;` `&&` `||`) since SH6c; `if`/`while`/`for`/`break` since SH6d. |
| `/hello` | ✅ | Smoke test app. |
| `/calc` | ✅ | Calculator. |
| `/sysinfo` | ✅ | Feature info display. |
| `/editor` | ✅ | Simple line editor. |
| `/clock` | ✅ | Uptime/countdown demo. |
| `/guess` | ✅ | Guessing game. |
| `/snake` | ✅ | Terminal snake. |
| `/http` | 🧪 | Uses DNS/TCP syscalls. |
| `/browser` | 🧪 | Text rendering of simple HTTP/HTML responses. |
| `/gcalc`, `/gedit`, `/gfiles`, `/gterm`, `/gsysmon`, `/gabout`, `/glaunch`, `/gusb` | 🧪 | GUI apps using `libauragui` v2.0; `/gusb` is the USB Manager for hotplug/storage status via `/usb`. `/gtheme` customizes window colors. |
| `/gbrowser` | 🧪 | GUI browser (WEBVIEW_PLAN **W0–W8 complete**, renamed from `/apps/webview` in W8): tokeniser, DOM, layout (img + form widgets), painting (reference hash 0xA29E776C host==guest), CSS (D4 subset), navigation (HTTP/1.1 **and HTTPS** via libahttp), `<canvas data-scene="cube">` (libgl FBO), **images** (PNG/JPEG/GIF/BMP, 8-slot fetch, `data:` URLs) and visible `<input>`/`<button>`/`<textarea>`/`<select>`. Chrome: Back/Fwd/Home/Go, address bar, hover-link status. See `docs/gbrowser.md`. |

## i386 (32-bit x86) — I386_PLAN

One image, two kernels: `make iso` ships `KERNEL.ELF` (x86_64) and
`KERNEL32.ELF` (i386); BIOS Stage 2 picks by CPUID at boot. The i386
kernel is a from-scratch sibling under `kernel/arch/i386/`, not a
recompile of the 64-bit tree — shared portable code migrates in through
`kernel/arch/arch.h` under the width-sweep ratchets
(`tools/check_width_sweep.py`). CPU floor: i686 (PSE/CX8/CMOV, plan D1);
older CPUs get an honest two-console refusal, 386 included.

| Feature | Status | Notes |
|---|---:|---|
| Long-mode check + refusal (BL10) | ✅ | EFLAGS.ID → CPUID → LM bit; refusal on COM1 **and** VGA. Silent-hang defect fixed in I0. |
| Dual-kernel boot chain | ✅ | `elf32.inc` + `pmode32.inc`; hand-off `ESI = boot_info_t`. Missing `KERNEL32.ELF` → refusal, not hang. |
| GDT / IDT / PIC / PIT / 32-bit TSS | ✅ | 256 gates, error-code parity per vector; `[diag]` dumps in the R0 format. |
| Non-PAE paging, higher half `0xC0100000` | ✅ | PSE 4 MiB direct map [0, 896 MiB) at `0xC0000000`; identity window dropped post-boot. |
| **No NX** | ❌ by design | Non-PAE has no NX bit (plan D3). W^X for user pages is *unenforceable*; stated in the boot log. |
| PMM / heap | ✅ | Shared `kernel/lib/bitmap.h`; E820 >4 GiB skipped-not-truncated (D6); same PASS self-tests as x86_64. |
| Preemptive scheduler | ✅ | Round-robin, post-EOI preemption, BSP-only (plan D5 — SMP stays x86_64). |
| Ring 3 + `int 0x80` | ✅ | AuraLite's own syscall numbers, Linux register convention (D4). Ring 3 faults contained, kernel survives. |
| ELF32 loader + initrd | ✅ | Shared `initrd.tar`, i386 binaries under `/bin32`; each kernel refuses the other's ELF class. |
| libc32 / init32 / shell | ✅ | `auralite#` interactive shell: SYS_READ (cooked), SYS_SPAWN (nested user images). Full libc port pending. |
| VGA text console + PS/2 keyboard | ✅ | Mode 3 at `0xB8000`; scancode set 1, shift. Framebuffer/VBE graphics: pending. |
| ATA PIO storage | ✅ | LBA28 on the boot controller; IDENTIFY + known-bytes read + write/readback/restore self-test. AHCI: waiting on a VFS consumer. |
| e1000 + DHCP/ARP/ICMP | ✅ | 82540EM; SLIRP lease + gateway ARP + payload-verified echo. Sockets/TCP/DNS: pending the net-stack port. |
| Crypto at 32-bit width | ✅ | RESIDUE R10: the COMPLETE suite (hash/AEAD/X25519/Ed25519/P-256, 104 checks) passes at `-m32` — `atls_fe.h` selects a packed 8×uint32 radix-2^32 field core and `atls_ecdsa.c` an 8×uint32 limb parameterisation where `__int128` is absent. Two lanes in `test_libatls_m32.sh`: `-DATLS_FE_FORCE32` on the native host (runs even without multilib) + the real `-m32` ABI. |
| VFS / FAT32 / ext2 / TCP / GUI | 🚧 | Residue tracked by ratchet 2 (x86_64-include count); lands as the shared subsystems finish the arch.h migration. |
| Rust userspace / w32 / USB / BT / Wi-Fi | ❌ | Per plan §6: no `i686-unknown-none` target; w32 is PE32+ by design; USB et al. deferred. |
| UEFI (`BOOTIA32.EFI`) | ❌ by design | BIOS/CSM only on i386 (plan D2). |

Tests: six-case `i386_*_smoke.sh` family (~100 assertions) beside
`cases/`, plus host gates (`test_width_sweep.sh`, `test_libatls_m32.sh`,
`test_boot_info_width.c` with its `-malign-double` negative control) and
`tools/check_i386_claims.py` tying I386_PLAN.md to the tree.

## RISC-V (rv64gc) — RISCV_PLAN

The third architecture: a from-scratch S-mode kernel under
`kernel/arch/riscv64/`, booted by OpenSBI on QEMU's `virt` machine
(`make kernelrv && make run-rv`). Same discipline as the i386 port —
shared portable code through `kernel/arch/arch.h`, one syscall table
(D4), one initrd (third tenant `/binrv`), the width-sweep ratchets
extended with ratchet 4 (`__asm__`-bearing portable files). CPU
floor: rv64gc (D1; no rv32).

| Feature | Status | Notes |
|---|---:|---|
| OpenSBI boot + DTB `boot_info_t` | ✅ | `-kernel` at `0x80200000`; the FDT shim is the struct's third producer. OpenSBI jumps to the payload BASE, not `e_entry` — enforced by `.text.boot` placement (measured in V0). |
| Traps / SBI timer / PLIC | ✅ | 16 named scause codes, FIX_R0 dumps; 100 Hz SBI timer; PLIC claim/complete proven with a real 16550 THRE interrupt. |
| Sv39 higher half + HHDM | ✅ | `0xFFFFFFC000000000` direct map; early tables are assembly-time data, satp on before any C runs. |
| **W^X enforced** | ✅ | Real PTE X bit: store-to-.text, execute-from-data and identity-window loads all FAULT (resumable probes). The i386 ❌'s green sibling. |
| Preemptive scheduler | ✅ | Round-robin, post-re-arm preemption, boot-hart only (D5 — SBI HSM is the SMP exit ramp). |
| U-mode + `ecall` | ✅ | AuraLite numbers, RISC-V Linux register convention (D4); sscratch trap-stack swap (the I7 lesson pre-paid); U-faults contained as 128+scause. |
| ELF64/EM_RISCV loader + initrd | ✅ | Three-way mutual class/machine refusal; `p_flags` become real PTE bits, W+X segments refused; `mkinitrd` audits all three tenants by `e_machine`. |
| libcrv / initrv / shell | ✅ | `smallsh` — ONE portable-C source shared with i386 (promoted from shell32.c); `auralite#` gate green on both. Full libc port pending. |
| virtio-mmio blk + net | ✅ | Legacy+modern transport over the 8 DTB windows; vrings shared with the PCI drivers (D7). ata32-shaped blk gate; DHCP/ARP/echo over the shared `miniproto`. VFS mount: pending. |
| 16550 UART RX over PLIC | ✅ | Interrupt-fed cons ring; the shell smoke asserts the `rx bytes via PLIC irq` receipt. |
| Crypto at rv64 | ✅ | The **complete** libatls suite (X25519/Ed25519/P-256 included — `__int128` exists here) executed under `qemu-riscv64`. Since R10 the `-m32` gate runs the same five suites through the 32-bit limb path; this lane keeps the `__int128` limb path honest per-ISA. |
| No rv32 | ❌ by design | Plan D1. |
| No own M-mode firmware | ❌ by design | Plan D2: SBI is the platform contract, like the BIOS was for Stage 2. |
| PCIe ECAM + virtio-pci | ✅ (R7) | The D7 deferral paid: shared `pci_ecam.c` walks bus 0 (`[pci] ECAM: N functions`), shared `virtio_pci.c` is the modern second transport (VERSION_1 acked), and vblk falls through to it when the mmio windows are empty — ext2 mounted over PCI, asserted in the rv_fs PCI lane. |
| SMP / vector ext / hypervisor ext | ❌ | Per plan §6; secondary harts parked safely via the boot lottery. |
| Rust userspace | ✅ (R8) | `rustes`/`rsbr` built for `riscv64gc-unknown-none-elf` from the SAME two sources (cfg'd ecall/rdtime), run from the initrd — the x86_64 receipt byte-exact, asserted in the rv_fs smoke; scounteren opened for the U-mode counter read. |

Tests: `rv_boot_smoke.sh` (46 assertions), `rv_shell_smoke.sh` (23),
`rv_parity_smoke.sh` (21, one boot, x86 pair attached), plus host gates
(`test_libatls_rv64.sh`, the third width in `test_width_sweep.sh`) and
`tools/check_riscv_claims.py` tying RISCV_PLAN.md to the tree. CI:
the `riscv-parity` job in `integration.yml`.

## ARM (aarch64 / ARMv8-A) — ARM64_PLAN

The fourth architecture: a from-scratch EL1 kernel under
`kernel/arch/aarch64/`, booted on QEMU's `virt` machine
(`make kernela64 && make run-a64`; the arm64 Image boot protocol via
`make run-a64-img`, which is also the `-initrd` path). Same
discipline, fourth application: shared portable code through
`kernel/arch/arch.h` (the DAIF irqflags backend closed over portable
code with ZERO portable-file edits — the D6 thesis, measured in A6),
one syscall table (D4, `svc #0`), one initrd (fourth tenant
`/bina64`), the shared DTB walker and virtio-mmio transport both
consumed from their promoted homes. CPU floor: ARMv8-A aarch64 at
EL1 (D1; no arm32, no EL2 entry).

| Feature | Status | Notes |
|---|---:|---|
| EL1 boot + DTB `boot_info_t` | ✅ | ELF `-kernel` (DTB parked at the RAM base, x0 measured ≠ DTB) AND the arm64 Image header (x0 IS the DTB, magic-verified before trust); the shared walker is the struct's fourth producer. |
| Traps / generic timer / GICv2 | ✅ | VBAR vectors with named EC codes; CNTV at 100 Hz from `CNTFRQ_EL0` (a register, not a DTB field); GIC claim/EOI proven with real interrupts; INTIDs pre-normalised by the walker (SPI+32/PPI+16 in ONE place). |
| TTBR1 higher half + HHDM | ✅ | 39-bit VA, `0xFFFFFFC000000000` direct map; TTBR0 blanked after init; MAIR: Device-nGnRE for MMIO, Normal WB for RAM — both alignment polarities MEASURED (Device faults, Normal succeeds). |
| **W^X enforced** | ✅ | PXN+UXN both ways: store-to-.text, execute-from-data, execute-from-user-data all FAULT (resumable probes); the ELF loader refuses W+X segments outright. |
| Preemptive scheduler | ✅ | Round-robin, post-EOI preemption hook, boot CPU only (D5 — PSCI `CPU_ON` is the named SMP ramp). FPU q-regs survive clobbering switches (eager save). |
| EL0 + `svc #0` | ✅ | AuraLite numbers, AAPCS64-friendly convention (D4); per-image dedicated trap stack; EL0 faults contained and named. The A5c measured lesson pinned: the trap frame does NOT carry `SP_EL0` — nested spawn saves/restores it explicitly. |
| ELF64/EM_AARCH64 loader + initrd | ✅ | Four-way mutual class/machine refusal, all three foreign tenants refused BY NAME in one live session (A8); `p_flags` become real PTE bundles; user stacks carry guard holes between nesting levels. |
| libca64 / inita64 / shell | ✅ | `smallsh` — the SAME portable source, fourth build, `--gc-sections` from birth; `auralite#` green with exit codes round-tripping. Full libc port pending (same residue class as rv64). |
| virtio-mmio blk + net | ✅ | The PROMOTED transport (`kernel/drivers/virtio_mmio.c` — one source, rv64 and a64 both link it); attach REFUSED over non-Device mappings (MAIR checked, Fact 5.2 by refusal); ata32-shaped blk gate; DHCP/ARP/echo over the shared `miniproto` (third consumer). |
| PL011 RX/TX over GIC | ✅ | RX: IRQ-fed cons ring (INTID 33 from the DTB), counted receipt (`rx bytes via GIC irq`) asserted by the smokes; TX: the O3 `uart_ring.h` index core under the A6 irqflags contract, drained before PSCI power-off. |
| Crypto at aarch64 | ✅ | The **complete** libatls suite (X25519/Ed25519/P-256 included) EXECUTED under `qemu-aarch64` — the second LP64 tenant through the `__int128` limb path (umulh edition; since R10 the `-m32` gate covers the 32-bit limb path). Deps named: `gcc-aarch64-linux-gnu`, `libc6-dev-arm64-cross`, `qemu-user`. |
| No arm32, no EL2 entry | ❌ by design | Plan D1: `CurrentEL != EL1` refuses with a banner (EL2 parks in `wfi` — an `hvc` from EL2 would trap into our own empty vectors); aapcs32 never enters the tree. |
| PCIe ECAM + virtio-pci | ✅ (R7) | The D7 deferral paid: the measured ECAM sits ABOVE 4 GiB on this board (VA carve, not HHDM folklore); shared walker + modern transport, vblk-over-PCI ext2 mount asserted in the a64_fs PCI lane. |
| SMP / SVE / big.LITTLE | ❌ | Per plan §4; PSCI `CPU_ON` is the recorded exit ramp (D5). |
| fw-cfg self-test knob | ✅ | RESIDUE R11 (RES-34): AMEND-5's deferral ends — `fwcfg_a64.c` is the MMIO reader (big-endian selector at +8, base from the DTB's `qemu,fw-cfg-mmio` node), feeding the same shared `selftest.c` as x86_64 and i386. `a64_boot_smoke` pins default-fast AND off→SKIPPED. |
| Rust userspace | ✅ (R8) | `rustes`/`rsbr` built for `aarch64-unknown-none` from the SAME two sources (cfg'd svc/cntvct), run from the initrd — the x86_64 receipt byte-exact, asserted in the a64_fs smoke; CNTKCTL_EL1 opened for the EL0 counter read. |

Tests: `a64_boot_smoke.sh` (41 assertions, ELF path),
`a64_image_smoke.sh` (12, Image protocol + initrd bytes),
`a64_shell_smoke.sh` (interactive EL0 session, log-size fuse armed —
the measured prompt-flood lesson), `a64_drivers_smoke.sh` (15, blk +
net + IRQ receipt), `a64_parity_smoke.sh` (26, one boot, every phase
gate, the refusal matrix row, x86 pair attached), plus host gates
(`test_libatls_a64.sh` EXECUTED, the fourth width and its compile
lanes in `test_width_sweep.sh`) and `tools/check_arm64_claims.py`
tying ARM64_PLAN.md to the tree. CI: the `aarch64-parity` job in
`integration.yml` (with the AMEND-6 toolchain-existence assert after
install).

## Real-hardware package + string-ops parity — HW_PLAN

The OPT_PLAN residue, paid (HW_PLAN.md, H0–H5).  Every hardware
feature here splits into a TCG-checkable correctness half (landed,
gated) and a metal performance half (a named receipt slot in
HW_PLAN §6 — TCG cannot measure it and the docs do not pretend).

| Feature | Status | Notes |
|---|---:|---|
| CPU feature receipts | ✅ | `[cpu] features: pat= pcid= invpcid= erms=` + `IA32_PAT` readback printed every boot on every lane — the receipt IS the compatibility matrix.  Measured: qemu64 has only PAT; `-cpu max` adds ERMS and still no PCID. |
| Word-wide portable string ops | ✅ | `kernel/lib/string.c` moves 8 bytes/iteration (may_alias word type — the strict-align-proof spelling); rv64 memcpy 413→2449 MB/s, a64 288→1964 (TCG).  rv64 adopted the shared file in H0; x86 keeps its rep-string backend (byte-identical control). |
| ERMSB crossover | ✅ TCG-half | Runtime, CPUID-fed: 64 (no ERMS, the O1-measured default) → 0 on ERMS parts; threshold line printed and pinned on both lanes.  Small-copy wall-clock: metal receipt. |
| PAT + WC framebuffer | ✅ TCG-half | PA4:=WC per-CPU (BSP + APs — attribute aliasing fenced by construction); fb HHDM range remapped exactly (huge pages split), PTE decode printed (`fb: WC via PAT4`).  gui shard pixel-green.  Flip throughput: metal receipt. |
| PCID | ✅ (TCG-inert) | RESIDUE R11: D-PCID-1..4 implemented — `pcid_policy.h` (pure C, host-tested: 24 checks) + `pcid.c`, CR4.PCIDE gated on CPUID, hash-slot allocation (named deviation from the bump-4095 write-up), NOFLUSH re-entry counted in `cr3_noflush_switches`, generation revocation in `pcid_generation_wraps`, the O5 sender filter generalised, handler de-owns non-resident victims — **no invpcid anywhere** (the user's WHPX machine has none). Measured this phase: TCG refuses `+pcid` outright, so every CI lane boots pcid=0 and the code is inert byte-for-byte; the perf smoke self-selects by the feature bit. The executable lane is the user's WHPX machine — `docs/metal_receipts.md` slots 5/6 are the D-PCID-5 acceptance. |
| rv64/a64 membench | ✅ | Boot-time bench of the LINKED string ops, verified passes, smoke-asserted on both tenants — the standing regression net for any future string-ops change. |

## Platform parity — PARITY_PLAN (P0–P9)

The catch-up series that turned three plans' shared residue into
receipts.  One block-device seam (`kernel/fs/blkdev.h`), four
consumers; ONE `ext2.c` mounting on every width the tree builds.

| Row | x86_64 | i386 | rv64 | a64 |
|-----|--------|------|------|-----|
| Filesystem (the SAME ext2.c) | ✅ AHCI via seam | ✅ ATA slave via seam | ✅ vblk via seam | ✅ vblk via seam |
| Storage driver → fs coupling | 0 (was 41 direct ahci calls) | 0 | 0 | 0 |
| Syscall surface | ~290 | 11 | 11 | 11 |
| Bring-up libc | full lib/libc | libcmini shim | libcmini shim | libcmini shim |
| SMP | ✅ scheduled | — (no ramp) | ✅ 15+1 @ -smp 16, IPI 15/15 | ✅ 15+1 @ -smp 16 (GICv3, R4) and 7+1 @ -smp 8 (GICv2) |
| SMP scheduling | per-CPU runqueues + stealing (receipt on AP) | — | 1 scheduled; R5: init RAN on a secondary hart | 1 scheduled; R5: init RAN on a secondary core |

Proof lanes in CI (`i386-parity` / `riscv-parity` /
`aarch64-parity`): `i386_fs_smoke` (13), `rv_fs_smoke` (18),
`a64_fs_smoke` (18), `rv_smp_smoke` (-smp 4 AND -smp 16),
`a64_smp_smoke` (-smp 8 — GICv2's architectural ceiling).  The
parity checker (`tools/check_parity_claims.py`) runs THREE live
compile lanes on every `make test-unit`: all kernel/fs files must
build as rv64, a64 AND i386 (`-Wshorten-64-to-32 -Werror`).

Width debt: the i386 pay-down (32 errors → 0) DELETED four
`(uint64_t)` casts — the sweep baseline clicked 359 → 355.

Named residue, carried forward (RES-13 PAID at R4: gic.c carries a
DTB-chosen GICv3 lane — redistributors, ICC sysregs, affinity SGIs —
and a64_smp_smoke proves -smp 16 at 15/15+15/15): tenant schedulers stay
single-CPU (per-CPU runqueues are their own series); i386
TCP/sockets (unchanged since I8); the full libc port (libcmini is
the floor, not the ceiling); vfs.c on the tenants (raw x86 `sti`
at vfs.c:71 + scheduler coupling — path-level VFS waits on it);
buffer_cache/tmpfs/devfs adoption rides the same blocker; the
i386 shell fd layer still serves the initrd (ext2 reaches the
shell with the VFS work); pit.h×2 + msc.h fs couplings (time/USB
seam questions, pinned per-file); GPT partitions (PCIe ECAM
paid at R7, the Rust rows at R8); one observed test_usb_hub TD-timeout
runner flake (1 occurrence, local repro green — remedy if it
recurs: guest-time TD waits).

## Known low-priority limitations

- **SMP scheduling (corrected by the residue ledger's R5 pass — this row was stale twice over).**  Per-CPU run queues, least-loaded placement and work stealing have been live since SMP 3.2, and user threads DO run on APs: the scheduler prints a one-time receipt (`[sched] R5 receipt: user thread pid=N on AP cpu=M`) and `test_fpu_smp` pins it.  Remaining named gap: a DEVICE interrupt waking a hlt-ed AP (ledger RES-16 — needs IOAPIC AP routing).
- **TCP is intentionally minimal.** TCP receive uses timed waits over the IRQ-capable e1000 driver and has basic one-segment fixed-RTO retransmission for SYN/data/FIN, but the stack still supports a small fixed number of streams and lacks production features such as congestion control, sliding windows and richer packet queues.
- **Advanced filesystems are prototypes.** `ext4`, `btrfs`, `f2fs`, `ntfs` and `exfat` are scaffolding/experimental readers, not robust general-purpose filesystem implementations; timestamp support is currently best covered in VFS/tmpfs/diskfs/FAT32/ext2.
- **Hardware coverage is mostly virtualized.** Integration coverage is QEMU-first, with some VirtualBox/VMware-oriented device IDs; broad real-hardware validation is still pending.

## Highest-priority gaps

1. Harden address-space teardown for future SMP/TLB-shootdown support.
2. Audit remaining kernel-internal callers and expand fault-recovering uaccess tests.
3. USB: U0–U7 and U9 are done and the fabricated data is gone (see [`docs/usb.md`](usb.md)). What remains is U8 — locking the event ring so the IRQ handler can drain it, then MSI/MSI-X — plus IRQ chaining in the kernel, which xHCI currently breaks for e1000 on a shared line.
4. Make scheduling SMP-aware or explicitly keep APs disabled in normal configs.
5. Grow `mmap` into lazy/shared VMAs and add broader file-backed sharing tests.
6. Tighten FD inheritance/lifetime semantics around `fork`, `execve` and process exit.
7. Expand shared/lazy VMAs and add guard pages around additional heap/stack regions.
