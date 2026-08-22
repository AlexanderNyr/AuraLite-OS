# The residue ledger — the living copy (RESIDUE_PLAN.md §2; R0 rig)

Machine-checked by `tools/check_residue_claims.py`: row count, id
sequence, class totals, status arithmetic.  Statuses: `OPEN`,
`DONE@Rn`, `RE-AFFIRMED@Rn`, `HANDED-OFF@Rn`.  A W row flips to
DONE only in the commit whose exit gate runs green; N rows die
loudly or live loudly (plan D4); S rows leave with a measured
opener fact (D5).

Class key: W = work (QEMU-provable now) · M = metal-only ·
N = non-goal to re-affirm · S = sub-series hand-off.

| ID | Class | Status | Item (source) | Exit gate |
|----|-------|--------|---------------|-----------|
| RES-01 | W | DONE@R1 | UHCI TD waits iteration-bounded; 1 runner flake seen (PARITY §5) | uhci.c waits on PIT ticks; usb shard green ×1 local |
| RES-02 | W | OPEN | `-cpu max` shell-banner oddity — R1 narrowed: shell STARTS, first SYS_WRITE never lands; ERMS and x2APIC exonerated by A/B boots | smoke explains it or the line appears |
| RES-03 | W | DONE@R1 | pit.h ×2 + msc.h couplings in kernel/fs, pinned per-file (PARITY P1) | seam decision note written; pins updated or removed |
| RES-04 | W | DONE@R1 | no partition parse: raw mounts silently ignore GPT/MBR (PARITY §6) | probe prints named skip on partitioned media; smoke pins it |
| RES-05 | W | DONE@R1 | TSS IST1 filled but no gate uses it (status.md) | double-fault lane uses IST1 with a receipt, or ist fill removed |
| RES-06 | W | OPEN | vfs.c raw `sti` @ vfs.c:71 blocks every port (PARITY P2) | vfs.c compiles for rv64/a64/i386 in the live lanes |
| RES-07 | W | OPEN | buffer_cache/tmpfs/devfs/cwd/symlink not adopted on ports (PARITY P2) | objects join the three shared lists; link green |
| RES-08 | W | OPEN | i386 shell fd layer initrd-only (PARITY P4/P7) | i386 shell cats /ext2 file through VFS; smoke pins it |
| RES-09 | W | OPEN | no path-level VFS mounts on rv64/a64 (PARITY P2) | `[vfs] mounted /` printed on both tenants; smokes pin it |
| RES-10 | W | OPEN | i386 TCP/sockets absent — net32 is miniproto only (I386 I8) | one TCP payload round-trips in an i386 smoke |
| RES-11 | S | OPEN | i386 compositor/GUI (I386 I8) | opener fact measured at R12 |
| RES-12 | S | OPEN | i386 VBE graphics; text mode is the console (I386 I7) | opener fact measured at R12 |
| RES-13 | W | OPEN | a64 -smp 16 needs GICv3; v2 = 8 ifaces architectural (PARITY P6) | a64_smp_smoke -smp 16 lane: 15 online, IPI 15/15 |
| RES-14 | W | OPEN | tenant SMP receipts-only; secondaries park (PARITY D5) | a user thread RUNS on a secondary; receipt counted |
| RES-15 | W | OPEN | x86 user scheduling BSP-only (status.md) | user thread observed scheduled on an AP; case pins it |
| RES-16 | W | OPEN | device IRQ waking a hlt-ed AP unproven (MATURITY) | receipt line in an SMP case |
| RES-17 | W | OPEN | full libc floor on ports: no malloc/stdio/TLS (RISCV V8 / PARITY P8) | port program mallocs + stdio round-trip, three ports |
| RES-18 | W | OPEN | PIE loading on ports waits on RES-17 (ARM64 close) | a PIE binary runs on one tenant; receipt |
| RES-19 | W | OPEN | userspace dynamic allocation needs brk/mmap (TODO.md) | brk/mmap-lite syscalls exist; pins move 11→N |
| RES-20 | W | OPEN | PCIe ECAM deferred on virt, both tenants (RISCV/ARM64 D7) | `[pci] ECAM: N functions` on rv64+a64 |
| RES-21 | W | OPEN | virtio-pci transport unused on tenants (TODO.md) | vblk-over-PCI mounts ext2 in a lane |
| RES-22 | W | OPEN | Rust row rv64: target exists, nothing built (RISCV close) | rustes/rsbr receipt line on rv64 |
| RES-23 | W | OPEN | Rust row a64: same class (ARM64 close) | same receipt on a64 |
| RES-24 | W | OPEN | IPv6 SLAAC/sockets/dual-stack recorded at X7 (REALINTERNET) | ping6 a SLAAC address; dual-stack fetch receipt |
| RES-25 | W | OPEN | TCP DNS fallback on truncated UDP (REALINTERNET X3) | >512B answer resolves via TCP; case pins it |
| RES-26 | W | OPEN | HTTPS-over-IPv6 fetch receipt missing (INTERNET N8) | one fetch receipt in a case |
| RES-27 | W | OPEN | /apps/http still hand-rolled, not libahttp (INTERNET) | app links libahttp; behavior pins hold |
| RES-28 | W | OPEN | virtio-net RX polls; IRQ RX pending (TODO/status; vmxnet3/e1000e data paths stay S → RES-46 class) | RX-via-IRQ receipt in net case |
| RES-29 | W | OPEN | atls_fe needs 32-bit limbs; -m32 crypto blocked (I386/status) | X25519/Ed25519/P-256 vectors green at -m32 |
| RES-30 | M | OPEN | PAT/WC framebuffer win measurable only on metal (OPT §7/HW H3) | R11 package line; user paste-back |
| RES-31 | W | OPEN | PCID: D-PCID-5 gate FIRED (user WHPX log `pcid=1`) — implement with CR3-toggle fallback (HW H4) | counters leave pinned zero; WHPX receipt block shipped |
| RES-32 | M | OPEN | ERMSB crossover tuning needs real silicon (HW H2) | R11 package line; user paste-back |
| RES-33 | M | OPEN | O3 wall-clock, O8 ThinLTO bar, membench metal numbers (OPT §7/HW §6) | R11 package lines |
| RES-34 | W | OPEN | fast-boot knob unwired on i386; a64 fw-cfg AMEND-5 deferred (OPT §7/ARM64) | knob toggles in a smoke on both |
| RES-35 | W | DONE@R1 | O1/O6/O8 measured at R1: O8 REFUSED with numbers (naive --gc-sections on kernelrv "saves" 68% by deleting live boot/trap sections — all five smokes red; needs a KEEP() audit, folded into the R12 hand-off notes); O1 superseded (P7 linked word-wide string.c on i386); O6 deferred to R6 where port allocation actually grows | measured; done or refused with numbers |
| RES-36 | S | OPEN | MSI/MSI-X for virtio + virtio-gpu (MATURITY) | opener fact at R12 |
| RES-37 | M | OPEN | IOAPIC base is QEMU-hardcoded; discovery is metal work (MATURITY) | R11 package line |
| RES-38 | S | OPEN | OHCI/EHCI/xHCI full transfer scheduling; HID beyond UHCI; BOT short-packet line (USB/TODO) | opener fact at R12 |
| RES-39 | S | OPEN | Bluetooth USB transport; Wi-Fi chipset backend (TODO/status) | opener fact at R12 |
| RES-40 | W | DONE@R1 | virtio-gpu init hang found by G13, bisected pre-G11d (TODO) | repro'd + fixed, or narrowed with a new fact |
| RES-41 | S | OPEN | TGSI backend for VirGL DRAW_VBO (GL G13) | opener fact at R12 |
| RES-42 | W | OPEN | POSIX leftovers: readline/scanf/jobs/epoll + posix2024 known_partials (POSIX/POSIX2024) | each entry triaged: scheduled or re-affirmed, at R12 |
| RES-43 | N | OPEN | W32 deferred W-surface, BitBlt, LoadLibraryW, kernel import binding (WIN32 D-numbers) | re-affirmed with D-numbers at R12 |
| RES-44 | N | OPEN | W32 registry (D8), MSG_COPY/SHM_LOCK/proc-sysvipc, DOOM §6, slab single-caller rule | re-affirmed at R12 |
| RES-45 | W | OPEN | TODO.md hygiene tail (fsck, writeback cache, AHCI breadth, GDB scripts, CI artifacts, spawn-timing) | every line dispositioned at R12; TODO points at ledger |
| RES-46 | S | OPEN | skeleton FS data paths (exFAT/NTFS/ext4/btrfs/f2fs 🚧) + vmxnet3/e1000e | opener fact at R12 |
| RES-47 | S | OPEN | GUI isolation/permissions, clipboard/focus, settings (TODO) | opener fact at R12 |
| RES-48 | M | OPEN | HW §6 metal receipts never exercised on user's machine | R11 package; pending-user is a status, not a failure |

## Arithmetic (checker-enforced)

Rows: 48.  Classes: **W 33 · M 5 · N 2 · S 8** (recounted by the
R0 rig; the plan §2 draft hand-summed 27/6/3/12 and was WRONG —
amended same-commit, catch recorded).  Statuses: OPEN 42, DONE@R1 6 (RES-01/03/04/05/35/40; 35 closed as measured-and-refused, 05 closed as stale-doc, 40 closed as no-longer-reproduces).

Harvest baseline lives in `tools/residue_baseline.txt` (the
harvester's own regex is the metric; the §2 draft quoted counts
from a wider grep that included "blocked" — superseded).  A marker
count that drifts from the baseline fails CI until the ledger and
baseline move in the same commit: new debt REGISTERS or the build
is red.
