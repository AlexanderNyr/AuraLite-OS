# tools/selfhost/Selfhost.mk -- in-guest shmake description (SELFHOST_PLAN.md SH6f).
#
# SELFHOST_TARGETS: kernel initrd iso user
#
# D5: that comment is the guest half of the target-set ratchet.  The host
# Makefile declares the same names in SELFHOST_TARGETS.  check_selfhost_claims.py
# compares them, so the two build descriptions cannot drift silently.
#
# D6: products live on /fat.  /tmp/build is scratch and does not appear here.
# Recipes are the SH6f driver proof (sh6e_stamp); SH7/SH8 replace them with
# tcc/aulink/mini-asm/mkinitrd without renaming the targets.
#
# Nine steps, so a kill+reboot can resume: phases 1-6 are independent stamps
# on /fat, 7 kernel, 8 initrd, 9 is the receipt printed by build.sh.
# Products have no mtime edges between them.  AuraLite's FAT clock
# (fat_now, 2-second field wrapping) is not monotonic, so a P6 → KERNEL
# prerequisite would rebuild KERNEL on the next boot even when both files
# already exist.  Existence is the resume signal: missing → build, present
# → skip.  SH8 can add real edges once the clock is monotonic.

CC  = /tests/sh6e_stamp
FAT = /fat

.PHONY: kernel initrd iso user phase6

# 1 worktree  2 config  3 libc  4 userland  5 asm  6 link
P1:
	$(CC) p1 P1

P2:
	$(CC) p2 P2

P3:
	$(CC) p3 P3

P4:
	$(CC) p4 P4

P5:
	$(CC) p5 P5

P6:
	$(CC) p6 P6

phase6: P1 P2 P3 P4 P5 P6

# 7 kernel  8 initrd  (9 = receipt in build.sh)
KERNEL:
	$(CC) kern KERNEL

kernel: KERNEL

INITRD:
	$(CC) init INITRD

initrd: INITRD

# SH7e: assemble the guest image in-guest.  The recipe runs the SH7a-SH7d
# twins in order and writes /fat/auralite.iso from the prebuilt /fat
# artefacts (SH5's /fat/KERNEL.ELF + the EFI loader) plus the initrd payload
# packed by /bin/mkinitrd.  Recipes are one exec per line (no shell, no
# redirects, no $(shell)/$(wildcard)), so each twin is its own recipe line
# and a failing line stops the build.  SH8 replaces /fat/KERNEL.ELF with the
# guest-tcc-built kernel; the target name does not change (D5).
iso: kernel
	sha256sum --selftest
	mkinitrd --selftest
	bootoffsets --check
	mkinitrd /fat/initrd-payload /fat/initrd.tar
	mkiso --esp-mb 48 --mbr /tests/mbr_dual.bin --stage2 /tests/stage2.bin --kernel /fat/KERNEL.ELF --efi /fat/BOOTX64.EFI --initrd /fat/initrd.tar /fat/auralite.iso

user: initrd
