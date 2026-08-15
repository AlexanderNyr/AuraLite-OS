#!/usr/bin/env python3
"""Cross-check the claims in docs/usb.md against the USB drivers.

In the manner of gen_w32_api_table.py --check: documentation that contradicts
the source is a build failure, not a stale paragraph nobody notices.

The point is asymmetric.  A claim that something WORKS is cheap to make and
expensive to trust, so each one is tied to a concrete artefact in the tree.
A claim that something is ABSENT is the one that rots quietly once it gets
implemented, so those are checked too -- in reverse.

Usage:
    tools/check_usb_claims.py [--check]

Exit status is non-zero if the documentation and the drivers disagree.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs", "usb.md")
USB = os.path.join(ROOT, "drivers", "usb")


def read(*parts):
    path = os.path.join(*parts)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def driver(name):
    return read(USB, name)


# (label, must_be_present_in_source, predicate)
#
# Each predicate takes the concatenated USB driver source and returns True
# when the claim holds.  Keep them narrow: a predicate that matches loosely
# is a predicate that will keep passing after the code stops being true.
def claims():
    xhci = driver("xhci.c")
    ehci = driver("ehci.c")
    core = driver("usb_core.c")
    hdr = driver("usb_core.h")

    return [
        # --- "what works" -------------------------------------------------
        ("xHCI Address Device is real",
         "xhci_address_device" in xhci and "Slot State=Addressed" in xhci),
        ("xHCI bulk transfers are real",
         "xhci_bulk_transfer" in xhci and "XHCI_TRB_NORMAL" in xhci),
        ("xHCI interrupt endpoints are real",
         "xhci_interrupt_transfer" in xhci and "ep_armed" in xhci),
        ("slots are freed on detach",
         "xhci_free_device" in xhci and "xhci_free_device" in core),
        ("EHCI interrupt endpoints use the periodic schedule",
         "ehci_periodic_link" in ehci and "periodic schedule" in ehci),
        ("nested hubs use a multi-tier location encoding",
         "usb_loc_child" in hdr and "USB_LOC_DEPTH_MAX" in hdr),
        # Check the TRB is actually *queued* as type 5, not merely that a
        # constant with that name exists.  The first cut of this predicate
        # tested for the substring "XHCI_TRB_ISOCH", which still matched
        # after the SIA define was renamed away -- a check that cannot fail
        # is not a check.
        ("isochronous queues a real Isoch TRB",
         re.search(r"XHCI_TRB_ISOCH\s*<<\s*XHCI_TRB_TYPE_SHIFT",
                   _strip_comments(xhci)) is not None and
         re.search(r"^#define\s+XHCI_TRB_ISOCH_SIA\b", xhci, re.M) is not None),
        ("the xHCI interrupt is taken",
         "irq_register_handler" in xhci and "xhci_irq_handler" in xhci),

        # --- "what is absent": these must STAY absent, or the doc is wrong -
        ("streams/UAS are still absent",
         "xhci_alloc_stream" not in xhci and "UAS" not in xhci.replace(
             "streams/UAS", "")),
        ("MSI/MSI-X is still absent",
         "pci_enable_msi" not in xhci and "MSI-X" not in xhci),
        ("the event ring is still drained by the consumer, not the IRQ",
         "xhci_ev_dequeue" in xhci and
         "xhci_ev_dequeue" not in _handler_body(xhci)),
        # Only live code counts.  The word AURALUSB still appears in a
        # comment describing the forgery U2 deleted, and that history is
        # worth keeping -- so strip comments before looking.
        ("no synthesised transfer data remains in live code",
         "SYNTHETIC" not in _strip_comments(xhci) and
         "AURALUSB" not in _strip_comments(xhci)),
    ]


def _strip_comments(src):
    """Drop /* ... */ and // comments, so claims are checked against code."""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _handler_body(xhci):
    """Return the text of xhci_irq_handler(), or '' if it is not there."""
    match = re.search(r"static void xhci_irq_handler\(.*?\n\}", xhci, re.S)
    return match.group(0) if match else ""


def main():
    doc = read(DOCS)
    if not doc:
        print("check_usb_claims: docs/usb.md is missing", file=sys.stderr)
        return 1

    failures = []
    for label, holds in claims():
        if not holds:
            failures.append(label)

    # The documentation must actually discuss the gaps it promises to track.
    for required in ("Streams / UAS", "MSI/MSI-X", "IRQ sharing",
                     "Event ring has no lock", "split transactions"):
        if required.lower() not in doc.lower():
            failures.append("docs/usb.md no longer mentions: %s" % required)

    if failures:
        print("check_usb_claims: documentation and drivers disagree\n",
              file=sys.stderr)
        for item in failures:
            print("  FAIL: %s" % item, file=sys.stderr)
        print("\nUpdate docs/usb.md, or the driver, so they agree.",
              file=sys.stderr)
        return 1

    print("check_usb_claims: %d claims verified against the drivers"
          % len(claims()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
