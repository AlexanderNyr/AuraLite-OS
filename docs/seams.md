# Seam decision notes (residue ledger RES-03; R1)

Two non-storage driver couplings live in `kernel/fs/` and are
PINNED per-file by `tools/check_parity_claims.py` (a new driver
include anywhere in fs fails even at the same total).  This note is
the decision the pins were waiting for.

## drivers/timer/pit.h in procfs.c and select.c — the TIME seam — LANDED @R6

Both consumers wanted one thing: a monotonic tick/uptime read.  The
right seam was a portable `kernel/time` READ interface.  DECISION
(made here at R1): fold into R6 — and R6 DID land it:
`kernel/time.h` provides `ktime_ticks()`/`ktime_hz()` with the bodies
in `kernel/time.c` (→ PIT on x86); procfs.c and select.c compile
with no driver includes, and both per-file pins dropped in that
commit (`DRIVER_INC_ALLOW` is usbfs.c-only since).  Kept here as the
record that the seam went exactly where this note said it would.

## drivers/usb/msc.h in usbfs.c — the USB seam

usbfs is an x86-only VFS window onto USB state; no port links it,
and the PARITY series showed no consumer needs it elsewhere.
DECISION: the pin STAYS as the honest record of an x86-only
coupling; it converts to a seam only when a second architecture
grows USB (the RES-38 sub-series' problem, not before).  Removing
the include today would be motion without progress.
