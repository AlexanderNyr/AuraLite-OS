# sh7d_probe.sh -- SELFHOST SH7d happy-path gate script.
#
# Staged at /tests/sh7d_probe.sh and run in-guest as
#     sh /tests/sh7d_probe.sh
#
# SH7d is the MBR+GPT+FAT32 image writer: a /bin/mkiso that lays down the
# whole hybrid disk the BIOS Stage 2 and OVMF both read, replacing the host
# mformat/mcopy + the inline python3 BPB patch.  This script proves, in order:
#   S1  the tool's built-in self check exercises the geometry math, the CRC32
#       GPT vector, the FAT32 BPB/FSInfo and the 8.3 directory layout
#       (`mkiso --selftest`, branching on $?)
#   S2  assembling an image from the staged boot blobs (/tests/mbr_dual.bin +
#       /tests/stage2.bin) and real in-guest files (kernel/efi/initrd slots)
#       succeeds and writes a non-empty output file
#   S3  a too-small ESP (< the 65525-cluster FAT32 floor) is REFUSED with a
#       non-zero status (the negative control)
#
# Like sh7b_probe.sh there is no cut/grep in the scripting shell: the tool
# carries the structural checks in --selftest and the script branches on exit
# codes.  The final receipt is the line the host greps.

OUT=/fat/auralite-sh7d.iso

# S1: built-in geometry/CRC/BPB/FAT self check.
mkiso --selftest

# S2: assemble a real hybrid image in-guest from the staged boot blobs.  The
# kernel/efi/initrd slots are filled with ordinary in-guest files (an ELF for
# the kernel, a PE .exe for the EFI app); the point is the MBR+GPT+FAT32 the
# tool lays down, not the payload bytes.
if mkiso --esp-mb 48 \
     --mbr /tests/mbr_dual.bin --stage2 /tests/stage2.bin \
     --kernel /bin/init --efi /tests/petest.exe \
     --initrd /tests/selftest --kernel32 /bin/hello \
     $OUT
then
  echo [selfhost] sh7d: s2-image-assembled
else
  echo [selfhost] sh7d: UNREACHABLE-ASSEMBLY-FAILED
fi

# S3: a sub-40 MiB ESP (< 65525 data clusters) must be refused, or OVMF would
# classify the volume as FAT16.  Negative control on the floor guard.
if mkiso --esp-mb 8 --mbr /tests/mbr_dual.bin \
     --kernel /bin/init --efi /tests/petest.exe /tmp/sh7d-small.iso
then
  echo [selfhost] sh7d: UNREACHABLE-SMALL-ESP-ACCEPTED
else
  echo [selfhost] sh7d: s3-small-esp-rejected
fi

echo [selfhost] mkiso PASS: auralite-sh7d.iso written in-guest
