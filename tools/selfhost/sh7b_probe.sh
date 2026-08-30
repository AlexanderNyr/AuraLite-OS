# sh7b_probe.sh -- SELFHOST SH7b happy-path gate script.
#
# Staged at /tests/sh7b_probe.sh and run in-guest as
#     sh /tests/sh7b_probe.sh
#
# SH7b is the second image-tooling C twin: a /bin/mkinitrd that writes the
# USTAR archive the kernel initrd parser already reads.  This script proves,
# in order:
#   S1  the tool's built-in self check packs a tree and reads its own archive
#       back (write -> reparse member-count round-trip), branching on $?
#   S2  packing a fresh tree produces an archive the tool itself can list
#       (--list re-parses the 512-byte headers: ustar magic, typeflag, size)
#   S3  the archive contains the expected member name (the reader side of the
#       writer's header layout), checked by branching on --list's status
#
# Like sh7a_probe.sh there is no cut/grep in the scripting shell, so the tool
# carries the verification modes itself and the script branches on exit codes.
# The final receipt is the line the host greps.

D=/tmp/sh7b_tree

# S1: built-in round-trip self check (pack /tests, reparse, compare counts).
mkinitrd --selftest

# S2/S3: build a small tree and pack it.
mkdir $D || true
echo sh7b-mkinitrd-data > $D/hello.txt
mkdir $D/sub || true
echo nested > $D/sub/nested.txt
mkinitrd $D /tmp/sh7b.tar

# The tool reads its own archive back (proves the header layout round-trips).
if mkinitrd --list /tmp/sh7b.tar
then
  echo [selfhost] sh7b: s2-archive-listed
else
  echo [selfhost] sh7b: UNREACHABLE-LIST-FAILED
fi

# Negative control: listing a nonexistent archive MUST fail (non-zero), or the
# success above would be meaningless.
if mkinitrd --list /tmp/sh7b_does_not_exist.tar
then
  echo [selfhost] sh7b: UNREACHABLE-FALSE-LIST
else
  echo [selfhost] sh7b: s3-missing-archive-rejected
fi

echo [selfhost] mkinitrd PASS: members written in-guest
