# build.sh -- SELFHOST_PLAN.md SH6f/SH7e: the in-guest entry point.
#
# Usage (the gates):
#     sh build.sh            # SH6f: build KERNEL + INITRD on /fat
#     sh build.sh kernel     # idem (the explicit SH6f gate form)
#     sh build.sh iso        # SH7e: assemble /fat/auralite.iso on /fat
#
# The worktree is /fat (D6).  /tmp/build is scratch and is gone after a
# reboot.  Products go through shmake so a run killed at phase 6 of 9
# continues from phase 7 rather than restarting: P1..P6 stay on /fat,
# KERNEL and INITRD are missing, shmake rebuilds only those.
#
# D10: there is no `test`/`[` builtin and no string comparison, so this file
# does not branch on the *value* of $1.  It hands the target to shmake and
# lets the recipe graph decide; `$?` (the sh runner stops on a failing line)
# is the only branch.  The SH6f pair is always (re)built so its receipt stays
# truthful; the SH7e `iso` target is assembled in addition when the caller
# names it.  `kernel initrd` are always named, so a missing $1 never leaves
# shmake with an empty target list (which would fall back to the file's first
# rule, P1) -- the extra target is simply the SH7e `iso` when given.
#
# An interrupted run is `shmake -C /fat phase6`, then this script again.
#
# SH7 replaces the iso recipe; SH8 replaces the stamp recipes with
# tcc/aulink/mini-asm.  The target names stay (D5).

mkdir /tmp/build || true
echo scratch > /tmp/build/tmp
mkdir /fat/src || true

# SH6f: the default pair.  `sh build.sh kernel` (or no arg) always yields the
# SH6f products; the optional caller-requested target (SH7e `iso`) is built
# after it.  A missing $1 expands to nothing, so shmake builds kernel+initrd.
shmake -C /fat kernel initrd $1

echo [selfhost] build PASS: kernel+initrd built on /fat
