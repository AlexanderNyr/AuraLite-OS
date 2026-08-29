# tools/selfhost/sh6e.mk -- SH6e gate Makefile (staged at /tests/sh6e.mk).
#
# A three-node graph: app depends on a.out and b.out, each of which
# depends on a source.  CC is a variable so the gate proves $(CC) expands;
# GEN is overridden on the command line so rebuild lines from run 1 and
# run 2 are greppable apart.
CC = /tests/sh6e_stamp
GEN = 0

app: a.out b.out
	$(CC) $(GEN) app a.out b.out

a.out: a.in
	$(CC) $(GEN) a.out a.in

b.out: b.in
	$(CC) $(GEN) b.out b.in
