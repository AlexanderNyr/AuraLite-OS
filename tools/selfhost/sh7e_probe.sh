# sh7e_probe.sh -- SELFHOST SH7e terminal gate: assemble the guest ISO in-guest.
#
# Staged at /tests/sh7e_probe.sh and run in-guest as
#     sh /tests/sh7e_probe.sh
#
# SH7e is the first end-to-end proof of Stage 1: `sh build.sh iso` runs the
# SH7a-SH7d twins in order and produces /fat/auralite.iso in-guest, which the
# host then boots (test_selfhost_iso.sh).  No code is compiled here -- SH7e
# only assembles the image from prebuilt artefacts on /fat; SH8 does the
# bootstrap closure.
#
# What each step has to prove:
#   S1-S4  the four twins are on the search PATH and self-check cleanly, in
#          SH7a-SH7d order: sha256sum (--selftest FIPS vectors), mkinitrd
#          (--selftest round-trip), bootoffsets (--check against the
#          host-generated header), mkiso (--selftest CRC32/geometry/BPB/FAT).
#          These need no guest toolchain, so they never skip on a plain
#          `make iso`.
#   S5     `sh /fat/build.sh iso` drives the Selfhost.mk `iso` recipe, which
#          runs the twins in order and writes /fat/auralite.iso.  mkiso owns
#          the structural check; build.sh returns nonzero (and the sh runner
#          stops the script) if any recipe line fails, so the SH7e receipt
#          below only prints after a clean assemble.
#
# Assembly inputs.  The `iso` recipe splices /fat/KERNEL.ELF, /fat/BOOTX64.EFI
# and a /fat/initrd payload dir.  In the full gate SH5 has already compiled
# /fat/KERNEL.ELF (the guest-built bootable kernel); this script stages the
# SH7d-proof stand-ins (the same shapes sh7d_probe.sh uses) so the chain is
# provable on a plain `make iso` when no SH5 kernel is present.  Booting the
# resulting image to a shell is the SH5/SH8 dependency; here the point is the
# assembly chain and its receipt.

# ---- stage the /fat worktree the recipe reads ----
# No `cp` in the AuraLite scripting shell and no /bin/cp binary, so a copy is
# `cat < src > dst` (the SH6b redirect idiom).  `mkdir` is a builtin that takes
# ONE path with no `-p`, so each payload directory is created on its own line.
mkdir /fat/src || true
cat /tests/sh6f.mk > /fat/Makefile
cat /tests/build.sh > /fat/build.sh

# ---- stage the assembly inputs (SH7d-proof stand-ins) ----
cat < /bin/init         > /fat/KERNEL.ELF
cat < /tests/petest.exe > /fat/BOOTX64.EFI
# the initrd payload directory the iso recipe packs then splices as INITRD.TAR
mkdir /fat/initrd-payload || true
mkdir /fat/initrd-payload/bin || true
mkdir /fat/initrd-payload/tests || true
cat < /bin/init  > /fat/initrd-payload/bin/init
cat < /bin/hello > /fat/initrd-payload/bin/hello

# ---- S1-S4: the twins self-check, in SH7a-SH7d order ----
sha256sum --selftest
mkinitrd --selftest
bootoffsets --check
mkiso --selftest
echo [selfhost] sh7e: twins-in-order

# ---- S5: the production recipe assembles /fat/auralite.iso in-guest ----
sh /fat/build.sh iso

echo [selfhost] iso PASS: auralite.iso built in-guest
