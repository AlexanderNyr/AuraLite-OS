# build.sh -- SELFHOST_PLAN.md SH6f: the in-guest entry point.
#
# Usage (the gate):  sh build.sh kernel
#
# The worktree is /fat (D6).  /tmp/build is scratch and is gone after a
# reboot.  Products go through shmake so a run killed at phase 6 of 9
# continues from phase 7 rather than restarting: P1..P6 stay on /fat,
# KERNEL and INITRD are missing, shmake rebuilds only those.
#
# There is no `test`/`[` builtin, so this file does not branch on $1.
# `sh build.sh kernel` is the documented API; extra words are ignored.
# An interrupted run is `shmake -C /fat phase6`, then this script again.
#
# SH7 replaces the iso recipe; SH8 replaces the stamp recipes with
# tcc/aulink/mini-asm.  The target names stay (D5).

mkdir /tmp/build || true
echo scratch > /tmp/build/tmp
mkdir /fat/src || true

shmake -C /fat kernel
shmake -C /fat initrd

echo [selfhost] build PASS: kernel+initrd built on /fat
