# AuraLite OS — OTA Update Plan (A/B kernel slots on the boot ESP)

## Status: IN PROGRESS — O0 ✅ O1 ✅ O2 ✅ DONE; O3–O5 pending

> This is a feature plan in the style of `FSFULL_PLAN.md`, `SELFHOST_PLAN.md`
> and `INTERNET_PLAN.md`, written against the tree as it stands. It follows
> the same structure: dependency-ordered phases, a definition of done and a
> test gate for every phase, one `.patch` per phase.
>
> It opens the OS's first self-maintenance feature: **the running system
> downloads a newer kernel over the network, installs it on the volume it
> booted from, and reboots into it — with a bootloader-level fallback that a
> bad update cannot brick.**
>
> The plan's thesis, measured below: **the network and FAT legs already
> exist** (an HTTPS client with certificate validation, full FAT32 mutation
> ops, `sync(2)`); what does not exist is the *boot-volume leg* — the kernel
> cannot even see the partition it booted from, and today it would actively
> corrupt it. Fixing that is not just enabling work; it closes a real
> destroy-the-boot-disk hazard that the measurements in §1.1 reproduce.

**Baseline:** commit `e927a76` (CI fix 4; CI run 92510954320: all 15 jobs
green — the CI-fix series is closed, the tree is a clean starting point).

**Ledger integration:** each phase's exit gate adds or extends a
machine-checkable CI case; the final phase (O5) registers the coverage rows
(`docs/residue_ledger.md`) and moves the baseline same-commit, exactly like
FSFULL's F7. The ledger stays the index of truth; this document is the
detail.

---

## 1. Where things actually stand

Measured on the baseline tree, not assumed from headers.

### 1.1 The boot volume is invisible — and worse, it gets formatted

* `kernel/fs/fat32.c:parse_or_format()` mounts **a fixed LBA 64** of blkdev 0
  (`FAT_DEFAULT_BASE_LBA 64u`, fat32.c:42). There is **no partition-table
  parsing at all**.
* The bootable image we actually ship — `build/auralite.iso`, the hybrid
  BIOS/UEFI disk — carries its ESP (the FAT32 volume with `KERNEL.ELF`)
  at **LBA 256** (measured: MBR part0 `type=0x0C start_lba=256
  sectors=98304`, GPT entry `C12A7328-…` "AURALITE ESP"; FAT32 magic at
  LBA 256, *not* at LBA 64).
* **Reproduced hazard (AHCI-boot experiment, baseline tree):** attach the
  dual image as the only AHCI disk and boot it (SeaBIOS boots AHCI fine —
  that part works). The kernel's fat32 looks at LBA 64, finds the GPT
  partition array (zeros), decides "empty disk", and **auto-formats
  8192 sectors at LBA 64..8255** — a range covering the GPT array, the
  stage2 binary (LBA 34..159) and the first ~4 MiB of the ESP. Under
  `snapshot=on` QEMU hides this from the host file; on real hardware it
  would destroy the boot chain on the first boot from SATA/AHCI.
* The F1 mount-safety knob (`FS_MOUNT_FORMAT`, fsformat.h) covers the five
  experimental filesystems only. fat32 was excluded *by design* — the
  "mature slot" test disks (il_make_disk: 55AA, no partition entries,
  zeros at LBA 64) rely on the auto-format to create their scratch /fat
  (test_selfhost_kernel_guest's whole SH5d flow formats its volume this
  way). The knob asymmetry is therefore **kept**, and the safety rule is
  narrower and sharper: **a disk that carries a partition table is never
  scratch** — no table-less empty area inside it may be formatted.

**O1 is therefore a correctness fix first and an enabler second.**

### 1.2 What already exists and is reused, not rebuilt

| Capability | Where (measured) | OTA use |
|---|---|---|
| HTTP/1.1 + HTTPS/TLS 1.3 client, cert-chain validation, redirects, keep-alive | `lib/libahttp` (`ahttp_client_get`, http.h; the `/http` app is a 129-line shell around it) | manifest fetch |
| SHA-256 (streaming) | public API in `lib/libatls/include/atls/atls.h` (`atls_sha256_init/update/final`) | payload integrity |
| FAT32 mutation: create/write/mkdir/unlink/**rename**/truncate | fat32.c ops table (:1518–1525); kernel self-test covers LFN/subdir/rename | A/B slot swap |
| Whole-cache flush | `sync(2)` / SYS_SYNC → `bc_flush_all` (RESIDUE2 CI wave 2) | durability before reboot |
| Stage 2 headroom | stage2.bin = 6144 B of the 64512 B the MBR can load (mkisoimage_dual.sh:79) | KERNEL.OLD fallback fits with ~58 KiB to spare |
| AHCI boot | SeaBIOS boots the hybrid image attached as `-device ahci -device ide-hd` (proven in the §1.1 experiment; boot disk == AHCI blkdev 0 == /fat) | the OTA test harness boots *the real boot disk* |
| Host-side HTTP fixture | test_http_get.sh's `python3 -m http.server` + SLIRP 10.0.2.2 pattern | the update server |
| Boot-time identity line | `[kernel] AuraLite version %s` (kernel.c:229, `AURALITE_VERSION` kernel.h:7) | second-boot proof |

### 1.3 The gaps this plan closes

1. **fat32 is partition-blind** (§1.1) — O1.
2. **No reboot** exists anywhere: no syscall, no shell command (grep across
   syscall.c / init.c: zero hits) — O2.
3. **The version string is hardcoded** (`"0.0.1"` in kernel.h *and* again in
   init.c's banner), so an updated kernel cannot be told from the old one at
   runtime — O2 adds a build-time override, the honest minimal identity.
4. **Stage 2 halts on an unloadable KERNEL.ELF** (the `.fat_no_kernel`
   path prints and halts; there is no second chance) — O3 adds the
   KERNEL.OLD fallback.
5. **AHTTP_MAX_BODY is 1 MiB** (http.h:29) while the kernel image is
   ~1.4 MiB — the payload must stream to disk, not buffer; the manifest
   (a few hundred bytes) is what goes through ahttp — O4.
6. No `ota` tool, no `sha256sum` in the main image (the selfhost world has
   one; the clang-built userspace does not) — O4.
7. Nothing wires the above into CI — O5.

### 1.4 Trust model (stated, not implied)

* The **manifest** is fetched through ahttp — HTTPS with full certificate
  validation against the shipped trust store in any real deployment, plain
  HTTP on the CI fixture (a local `python3 -m http.server` cannot do TLS;
  the case names that downgrade explicitly).
* The **payload** is fetched over plain HTTP but is authenticated by the
  **sha256 carried in the manifest** — a TUF-lite shape: the small document
  is the authenticated thing, the large blob is verified against it.
* Streaming TLS for the payload, ECDSA-signed manifests (atls already has
  the primitives), delta updates and a boot-success watchdog (counter file
   consulted by stage 2) are **named deferrals**, §2.

---

## 2. Non-goals / deferrals (parked, each with a reason)

* **Streaming TLS payload** — needs an incremental ahttp API; the manifest
  sha256 already buys integrity, and the manifest itself can be HTTPS.
* **Signed manifests** — atls carries ECDSA/RSA; pinned-key signature
  verification is a clean follow-up once the flow exists, but it adds key
  management this plan does not need to be honest about.
* **initrd / userspace image updates** — the manifest format gains an
  `artifacts=` list later; kernel-first is where the risk and the payoff
  are.
* **Boot-success watchdog** (stage 2 consulting a boot-attempt counter to
  auto-rollback a kernel that triple-faults mid-boot) — stage 2 can read
  files, so it is possible, but the A/B fallback plus `ota rollback`
  covers the corrupt/unloadable class first; auto-rollback heuristics
  deserve their own measured design.
* **Delta/binary-diff updates** — bandwidth optimisation, not correctness.

---

## 3. Phases

### Phase O1 — Boot-volume correctness: partition-aware FAT32 mount ✅ DONE

**Objective:** the kernel sees the volume it booted from, and can never
format inside a partitioned disk.

#### Tasks
- [x] `find_fat_base()`: read LBA 0/1 once; if a GPT header ("EFI PART")
      exists, scan its entries; else if the MBR has 55AA **and at least one
      non-empty entry**, scan those (a bare 55AA with zero entries is the
      il_make_disk scratch shape, not a table); the first entry whose start
      LBA carries FAT32 magic wins and mounts as **base_lba = entry start**.
- [x] A partitioned disk with **no** FAT32 volume anywhere refuses with a
      greppable line (`[fat32] partitioned disk has no FAT32 volume; not
      formatting a partitioned disk`) and mounts nothing — **no format path
      may run on a table-bearing disk**, knob or no knob.
- [x] Table-less disks keep today's semantics exactly (LBA 64 raw: magic →
      mount, else auto-format) — the mature-slot contract the selfhost
      suite depends on.
- [x] Boot receipt: `[fat32] found FAT32 partition at LBA %u (via %s)` —
      `GPT`/`MBR`/`raw` — so tests and humans can see which leg ran.

#### Test gate
- `tests/integration/cases/test_ota_bootvol.sh`, three lanes: (A) boot
  the **dual ISO via AHCI as the only disk** and assert the `found FAT32
  partition at LBA 256 (via GPT)` receipt, `ls /fat` lists `KERNEL.ELF`,
  zero `formatting` lines in the whole boot, and the AHCI self-test's
  non-destructive DMA receipt (the `via GPT` mount proves the GPT header
  survived the boot byte-for-byte — a host-side image hash would prove
  nothing, the QEMU drive is snapshot-backed); (B) a table-bearing disk
  with no FAT32 volume refuses and formats nothing; (C) a table-less
  scratch disk still auto-formats at LBA 64 and mounts (the SH5d
  contract, now pinned by its own lane instead of living only inside
  the selfhost suites).
- Regression lanes green: test_selfhost_kernel_guest (scratch-disk format
  semantics unchanged), test_fat32_full, test_fsformat_knob, the ahci
  lanes (rw, matrix incl. q35, large_read) whose PASS receipts the
  self-test change must keep intact.

**Result:** implemented and green — see the O1 Result section at the bottom.

#### Deliverable
`patches/OTA_O1_boot_volume.patch` ✅

---

### Phase O2 — Reboot and identity ✅ DONE

**Objective:** the OS can restart itself, and a kernel build can carry a
distinct version string.

#### Tasks
- [x] `SYS_REBOOT` (612): 8042 pulse (`outb(0x64, 0xFE)`) with a
      documented QEMU interaction — under `-no-reboot` QEMU exits (clean
      test teardown), without it the machine resets and boots again on the
      same snapshot overlay (the OTA flow's second boot).
- [x] `reboot` shell command (init.c command table) with a receipt line.
- [x] `AURALITE_VERSION` becomes overridable (`#ifndef` in kernel.h) and
      the Makefile passes `-DAURALITE_VERSION='"$(AURALITE_VERSION)"'`;
      init.c's hardcoded banner reads the same macro instead of its own
      literal.
- [x] A version-override build (`make iso AURALITE_VERSION=0.0.2-ota`)
      boots and prints the distinct banner — this is the O4 proof vehicle.

#### Test gate
- `test_ota_reboot.sh`: boot, `reboot`, and **the same log file** shows the
  second boot's stage2/kernel banner (QEMU without `-no-reboot`, snapshot
  overlay persists within the process); second boot reaches the shell.
- `make test-unit` + existing suite green.

**Result:** implemented and green — see the O2 Result section at the bottom.

#### Deliverable
`patches/OTA_O2_reboot.patch` ✅

---

### Phase O3 — Stage 2 A/B fallback

**Objective:** an unloadable or missing KERNEL.ELF degrades to KERNEL.OLD
instead of halting.

#### Tasks
- [ ] stage2_start.asm: on `fat_find(KERNEL.ELF)` miss **or** `elf_load`
      failure, `fat_find(KERNEL.OLD)` → `fat_load` → `elf_load`, with the
      receipt `[BL4] KERNEL.ELF unloadable -- falling back to KERNEL.OLD`
      (and the happy path unchanged, zero new output).
- [ ] The fallback is bounded: one retry, no loop; both failing keeps the
      existing loud halt.
- [ ] Stage 2 stays within the 126-sector budget (currently 6144/64512 B).

#### Test gate
- `test_ota_fallback.sh`: a host-mtools-doctored copy of the dual image
  with KERNEL.ELF replaced by garbage boots via the OLD slot (receipt +
  shell reached); an untouched image never prints the fallback line.

#### Deliverable
`patches/OTA_O3_fallback.patch`

---

### Phase O4 — The `ota` tool and `sha256sum`

**Objective:** the full check → download → verify → swap → sync → reboot
flow, driven from the shell.

#### Tasks
- [ ] `userspace/apps/ota/ota.c` (linked against libahttp + libatls):
  - `ota check <manifest-url>` — fetch via ahttp (HTTPS-capable), parse
    the line-based manifest (`version=`, `url=`, `size=`, `sha256=`),
    print the plan; refuse on missing fields or size > free ESP space.
  - `ota apply <manifest-url>` — stream the payload with a **minimal
    in-app HTTP GET** (socket → send → parse headers → read body in 4 KiB
    chunks straight into `/fat/KERNEL.NEW`, `atls_sha256_update` on the
    fly — nothing ever buffers 1.4 MiB), verify the digest, then
    `rename("/fat/KERNEL.ELF", "/fat/KERNEL.OLD")`,
    `rename("/fat/KERNEL.NEW", "/fat/KERNEL.ELF")`, `sync()`, and print
    per-step receipts (`[ota] payload verified (sha256 ok)`,
    `[ota] A/B swap done: KERNEL.OLD <- current, KERNEL.ELF <- new`,
    `[ota] synced; reboot to activate`).
  - `ota rollback` — swap OLD/ELF back, `sync()`.
  - `ota status` — size + sha256 of the active and (if present) OLD slot.
- [ ] `sha256sum` app (the main image's userspace has none; atls provides
  the primitive) — useful beyond OTA and the honest way the test verifies
  files on the guest.
- [ ] Manifest parser + digest verify as **unit tests** with NIST vectors
  (`tests/unit/test_ota_manifest.c`).

#### Test gate
- `test_ota_apply.sh` (the plan's exit-quality gate): build a
  version-override kernel (`0.0.2-ota`), serve it + manifest with the
  http-server fixture, boot the dual ISO via AHCI, `ota apply`, `reboot`;
  the **second boot** shows `version 0.0.2-ota`; `ota rollback` + reboot
  shows `0.0.1` again. Every step asserted by receipt.
- Negative lane: a tampered sha256 in the manifest aborts with
  `[ota] sha256 MISMATCH` and leaves KERNEL.ELF untouched.

#### Deliverable
`patches/OTA_O4_ota_app.patch`

---

### Phase O5 — CI wiring and close-out

**Objective:** the flow is a first-class, machine-checked citizen.

#### Tasks
- [ ] `test_ota_bootvol/reboot/fallback/apply` registered in
      `run_all.sh` ALL_CASES and placed on a shard (net lane: it already
      carries the http-server fixture pattern).
- [ ] `docs/status.md`, `README.md`, `CHANGELOG.md`, TODO row, ledger
      coverage rows; `tools/check_*_claims.py` extended if the docs make
      new claims (they will: "OTA A/B with bootloader fallback" is a
      checkable claim).
- [ ] The plan's Status line flips to COMPLETE; deferrals restated in the
      ledger with their §2 reasons.

#### Test gate
- CI run: all jobs green with the new cases included.

#### Deliverable
`patches/OTA_O5_wireup.patch`

---

## 4. Exit gate (the whole plan, one paragraph)

A CI case boots the real bootable image from AHCI, over the network
installs a differently-versioned kernel onto the partition it booted from,
reboots, and the same serial log proves the second boot runs the new
version; corrupting either slot still boots via the other. That is OTA:
measured, asserted, no forgery possible (the version banner is printed by
the kernel that actually booted, and the payload digest is checked against
the manifest before anything is renamed).

---

## O1 Result

Implemented, measured, green. Implementation found the hazard was wider
than the O0 survey: **three** blind writers hit the first AHCI disk's
fixed LBAs during boot, and all three are closed by the same rule —
*a partitioned disk's sectors are never scratch*:

1. `kernel/fs/fat32.c` — `find_fat_base()` now consults the disk's own
   tables via the canonical `blkdev_partition_kind()` probe (a bare 55AA
   with zero entries stays the table-less scratch shape): GPT entries
   first (header at LBA 1), then non-protective MBR entries; the first
   entry whose start LBA carries FAT32 magic mounts as the volume
   (`[fat32] found FAT32 partition at LBA 256 (via GPT)` on the dual
   image). A table-bearing disk with no FAT32 volume refuses loudly
   (`[fat32] partitioned disk has no FAT32 volume; not formatting a
   partitioned disk`) and nothing formats. Table-less disks keep the
   raw-LBA-64 semantics to the byte, auto-format included.
2. `kernel/fs/diskfs.c` — the tiny-AUFS auto-format at LBA 2 sat inside
   the dual image's GPT entry array and destroyed it before fat32 ever
   looked (this is why the first boot experiments degraded to `via MBR`:
   diskfs had already wiped the entries). It now refuses on table-bearing
   disks: `[diskfs] partitioned disk is not an AUFS scratch disk`.
3. `drivers/ahci/ahci.c` — the AHCI self-test's scratch write stamped
   `AURALAHCI-WRITE` over LBA 1, i.e. over the GPT header itself
   (measured: `EFI PART` was gone at mount time on the baseline tree —
   and the q35 matrix lane puts the boot image itself on an AHCI port,
   so it has been trashing its own boot disk's table on every run,
   invisible because the IDE attach is snapshot-backed). The DMA write
   path is still verified on every disk, but non-destructively on
   partitioned ones: save the sector, write the pattern, read it back,
   restore, verify the restore — the `[ahci] PASS: SATA read/write DMA`
   receipt every existing lane greps for is unchanged.

Gate `tests/integration/cases/test_ota_bootvol.sh` (11 assertions, 3
lanes): booting the real dual image from AHCI as the only disk mounts
the ESP via GPT, lists `KERNEL.ELF` in `/fat`, shows zero `formatting`
lines; a table-bearing disk with no FAT volume refuses; a table-less
scratch disk still auto-formats at LBA 64 and mounts (the SH5d
contract). Regressions green on the patched tree (noble clang 18.1.3,
the CI compiler): selfhost_kernel_guest 26/26 (guest-built kernel
booted from the raw LBA-64 volume — scratch semantics untouched),
fat32_full 12/12, ahci_rw 9/9, ahci_matrix 17/17 (the q35 lane now
passes with its boot disk's table intact), ahci_large_read 8/8,
fsformat_knob green, `make test-unit` EXIT 0 with the baseline and
ledger moved same-commit (the plan doc itself carries 6 marker lines;
O1 added none).

## O2 Result

Implemented, measured, green.

1. `SYS_REBOOT` (612) in `kernel/arch/x86_64/syscall.c`: flush
   filesystems first (`fs_cache_sync`), receipts
   `[reboot] SYS_REBOOT: flushing filesystems` and
   `[reboot] pulsing 8042 reset`, then `outb(0x64, 0xFE)`.  Measured QEMU
   behaviour, now relied upon by the gate: WITHOUT `-no-reboot` the machine
   resets onto the same in-process snapshot overlay, so one serial log
   carries both boots; a pulse that a platform ignores ends in a loud
   halt (never a silent return).
2. `reboot` shell command (`userspace/system/init/init.c`): `reboot()` in
   libc (`SYS_REBOOT` wrapper beside `sync()`), command-table entry, help
   line, and a "the machine did not reset" line that is only reachable
   if the reset failed.
3. `AURALITE_VERSION` is one build knob: `make iso
   AURALITE_VERSION=0.0.2-ota` passes `-DAURALITE_VERSION='"..."'` to
   every kernel AND userspace compile (`CFLAGS` + `USER_CFLAGS`); the
   `#ifndef` fallbacks (kernel.h, init.c, utsname.c) keep non-Makefile
   compiles — including the guest tcc selfhost build, whose `uname`
   assertion still greps the stock string — on the stock identity.
   Implementation found three identity prints beyond the plan's named
   two: `/proc/version` (procfs.c), the graphics-mode banner (kernel.c)
   and `uname(2)`'s release field (utsname.c); all read the macro now,
   so an override build is coherent end to end (kernel receipt, shell
   banner, `uname`, `/proc/version`, gfx banner — all five).
   Stale-object trap, recorded for O4: object files do not depend on the
   Makefile, so an override on a warm tree can silently keep stock
   objects — the gate builds its override image COLD in a throwaway
   BUILD_DIR.

Gate `tests/integration/cases/test_ota_reboot.sh` (11 assertions, 2
lanes): lane A boots the dual image from AHCI WITHOUT `-no-reboot`, runs
`reboot`, and the SAME log shows two kernel identity receipts, two
"Hello" banners, two ESP-via-GPT mounts (the O1 invariant holds across
the reset) and an answered `uname`; lane B cold-builds
`AURALITE_VERSION=0.0.2-ota` and asserts all three greppable prints
carry the override with no stale stock-version line anywhere.
Regressions green (noble clang 18.1.3): syscalls 4/4, shell_commands
9/9, boot_to_shell 17/17, selfhost_kernel_guest 26/26 (guest tcc build
unaffected — fallback identity), ota_bootvol 11/11, `make test-unit`
EXIT 0.
