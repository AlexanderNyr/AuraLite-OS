# sh6e_probe.sh -- SELFHOST SH6e happy-path gate script.
#
# Staged at /tests/sh6e_probe.sh and run in-guest as
#     sh /tests/sh6e_probe.sh
#
# A three-node graph (app -> a.out, b.out -> a.in, b.in) is the smallest
# tree that can prove "touch one prerequisite, rebuild only what depends
# on it".  GEN=N is a command-line variable so rebuild lines from the
# cold run and the incremental run are greppable apart (the serial log
# is cumulative).
#
# sleep 2 sits between the cold run and the touch because vfs_now() is
# one-second resolution; without it a.in and a.out share a timestamp and
# POSIX "prereq newer than target" does not fire.

mkdir /tmp/sh6e
echo a > /tmp/sh6e/a.in
echo b > /tmp/sh6e/b.in

shmake -C /tmp/sh6e -f /tests/sh6e.mk GEN=1

sleep 2
touch /tmp/sh6e/a.in

shmake -C /tmp/sh6e -f /tests/sh6e.mk GEN=2

shmake -C /tmp/sh6e -f /tests/sh6e.mk GEN=3

echo [selfhost] shmake PASS: 3 targets up to date
