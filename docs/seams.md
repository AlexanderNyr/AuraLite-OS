# Seam decision notes (residue ledger RES-03; R1)

Two non-storage driver couplings live in `kernel/fs/` and are
PINNED per-file by `tools/check_parity_claims.py` (a new driver
include anywhere in fs fails even at the same total).  This note is
the decision the pins were waiting for.

## drivers/timer/pit.h in procfs.c and select.c — the TIME seam

Both consumers want one thing: a monotonic tick/uptime read.  The
right seam is a portable `kernel/time` READ interface (the x86 tree
already has kernel/time.c; the ports have rdtime/cntvct/pit32).
DECISION: fold into R6 (libc v2 needs a clock for stdio timestamps
anyway) — the seam lands there as `ktime_ticks()`/`ktime_hz()`,
both pins drop in that commit.  Until then the pins stand guard.

## drivers/usb/msc.h in usbfs.c — the USB seam

usbfs is an x86-only VFS window onto USB state; no port links it,
and the PARITY series showed no consumer needs it elsewhere.
DECISION: the pin STAYS as the honest record of an x86-only
coupling; it converts to a seam only when a second architecture
grows USB (the RES-38 sub-series' problem, not before).  Removing
the include today would be motion without progress.
