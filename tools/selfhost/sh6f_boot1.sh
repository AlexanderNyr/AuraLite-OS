# sh6f_boot1.sh -- first boot of the SH6f gate.
#
# Stage the worktree on /fat, stop at phase 6 of 9, then resume through
# build.sh.  The serial log is cumulative, so the stop marker has to be
# a string that the resume path never reprints.

mkdir /tmp/build || true
cat /tests/sh6f.mk > /fat/Makefile
cat /tests/build.sh > /fat/build.sh

shmake -C /fat phase6
echo [selfhost] sh6f: stopped-at-6

sh /fat/build.sh kernel
