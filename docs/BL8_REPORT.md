# Phase BL8 — Remove Limine Dependency from Main Build — Completed

Result commit: `dbe6384` — `boot: BL8: default build no longer requires Limine`
Toolchain: unchanged from BL7 (clang 19.1, lld 19, lld-link 19, NASM 2.16, mtools 4.0, QEMU 10.0, OVMF, xorriso 1.5).

---

## Definition of Done — all criteria met

| # | Criterion | Result |
|---|-----------|:------:|
| 1 | `grep -rn 'limine_get_' kernel/` -> zero results | **0 hits** |
| 2 | `grep -rn 'limine/limine.h' kernel/` -> zero results | **0 hits** |
| 3 | `.limine_requests*` sections gone from `kernel.ld` | **✓ (since BL1)** |
| 4 | `-I limine` removed from `CFLAGS` | **✓** |
| 5 | `tools/mkisoimage.sh` renamed to `tools/mkisoimage_limine.sh` | **✓** |
| 6 | `make iso` no longer requires Limine | **✓ (chains to `iso-dual`)** |
| 7 | `README.md` updated: quickstart drops Limine, adds boot-paths section | **✓** |
| 8 | `make clean && make all` builds cleanly with 0 warnings | **✓** |
| 9 | `make test-unit` -> all tests green | **36/36 pass** |
| 10 | Legacy Limine ISO still buildable via `make iso-limine` | **✓ (target preserved)** |

---

## Files changed

| Path | Change |
|------|--------|
| `Makefile` | `-I limine` removed; `iso` chains to `iso-dual`; `iso-limine` added as explicit fallback; `mformat`/`mcopy`/`lld-link` promoted to REQUIRED_TOOLS |
| `README.md` | Rewritten intro, quickstart, boot-paths table, Limine-fallback section, extended make-target reference |
| `.github/workflows/integration.yml` | Added `ovmf`/`socat`/`dosfstools`; runs all eight BL* smoke tests explicitly; artefact upload extended to `build/bl*.log`/`bl*.mem` |
| `tools/mkisoimage.sh` | Renamed to `tools/mkisoimage_limine.sh` (verbatim) |

---

## What is intentionally preserved

The following pieces of the Limine plumbing are left in the tree so that
`make iso-limine` continues to work.  They are strictly optional: users
who never invoke `iso-limine` never touch them.

| Path | Why kept |
|------|----------|
| `limine-binary.tar.gz` | Optional input to `make iso-limine`. |
| `limine/` (vendored headers) | Header consumed by `mkisoimage_limine.sh`. |
| `third_party/limine` (submodule reference) | Alternate build source for `iso-limine`. |
| Makefile variables `LIMINE_*` and rule `limine-build` | Only reached by `iso-limine`. |

Removing them entirely is a follow-up cleanup that touches only these four
paths and would not need to modify the code base at all.

---

## Verification transcript

```console
$ make clean && make all
rm -rf build
...
  [mkiso-dual] wrote build/auralite-dual.iso (257M, dual-boot BIOS+UEFI hybrid image)
[release] Wrote ISO, kernel.elf, initrd.tar, and SHA256SUMS to 'release/' folder

$ grep -rn 'limine_get_'   kernel/  ; echo done
done
$ grep -rn 'limine/limine.h' kernel/  ; echo done
done
$ grep 'limine' kernel.ld ; echo done
done
$ grep -- '-I limine' Makefile ; echo done
done

$ make test-unit
...
test_boot_info: ALL PASS

$ for t in bl2_mbr bl3_stage2 bl4_fat bl4_elf bl4_boot bl5_iso bl6_uefi bl7_dual; do
    bash tests/integration/${t}_smoke.sh | tail -1
done
[bl2] PASS  MBR handed off to Stage 2 (LBA read succeeded)
[bl3] PASS
[bl4] PASS
[bl4-elf] PASS
[bl4-boot] PASS -- full BIOS boot chain works end to end
[bl5-iso] PASS -- BL5 hybrid ISO boots to kernel under BIOS (-drive if=ide)
[bl6-uefi] PASS -- BOOTX64.EFI boots to kernel via UEFI/OVMF
[bl7-dual] PASS -- one ISO boots to kernel via BOTH BIOS and UEFI
```

44 / 44 tests green (36 unit + 8 integration).

---

## Deviations from the specification

* **Limine sources not deleted.**  The spec BL8 suggests physically
  deleting `kernel/limine_requests.[ch]` (already done in BL1),
  removing `-I limine` (done here), removing `.limine_requests*`
  from kernel.ld (done in BL1) -- but leaves the fate of the
  binary bundle to the maintainer.  We chose to KEEP the bundle
  and headers so that `make iso-limine` remains a working
  fallback for firmware where the custom loader fails.  Nothing
  in the default build touches those paths.

* **CI job left `continue-on-error` on BL6/BL7/legacy tests.**
  Runner-image variation for OVMF paths (`/usr/share/OVMF/OVMF_CODE.fd`
  vs. `OVMF_CODE_4M.fd` vs. `OVMF_CODE_4M.secboot.fd`) makes strict
  gating fragile.  The tests themselves either PASS with a hard
  assertion or SKIP with a visible message; either outcome keeps
  CI green.  When the runner OVMF path stabilises upstream, drop
  the `continue-on-error` from those steps.

---

## Post-BL8 build map

```
make iso            -> build/auralite-dual.iso + release/auralite.iso
                       BIOS + UEFI dual-boot; no Limine required.
make iso-dual       -> build/auralite-dual.iso   (same, no release step)
make iso-bios       -> build/auralite-bios.iso   (BIOS-only, BL5)
make iso-limine     -> release/auralite-limine.iso  (legacy fallback)
make mbr / mbr-dual / stage2 / efi    -- individual loader artefacts
make kernel                            -- build/kernel.elf only
make test-unit                         -- 36 host-side tests
bash tests/integration/bl{2..7}_*.sh   -- eight QEMU integration tests
```
