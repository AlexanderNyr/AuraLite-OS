#!/usr/bin/env bash
# test_usb_hid_report_negcontrol.sh — RESIDUE3 C5b: does the new host test
# actually DISCRIMINATE?
#
# The plan's definition of done for C5b was "a HID case that fails against
# today's parser and passes after".  The old parser no longer exists in the
# tree, so it is reconstructed here, mechanically: drivers/usb/hid_report.c is
# copied into a scratch file and the two report-ID behaviours are reverted to
# the shape hid.c had before C5b --
#
#   * on a Report ID item the bit cursor keeps going (one cursor for the whole
#     descriptor instead of one per report), and
#   * the report-ID bookkeeping still records each ID, but the per-report body
#     length is therefore wrong, so wire_len and every field offset after the
#     first report are wrong as well.
#
# Then the SAME test file is compiled against the old-shape parser.  If the
# test still passed, it would be measuring nothing; the expected result is a
# failure, and this script fails if that failure does not happen.
#
# Usage: tests/unit/test_usb_hid_report_negcontrol.sh   (exit 0 = control held)

set -u
cd "$(dirname "$0")/../.."
ROOT=$PWD
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp drivers/usb/hid_report.h "$TMP/"
python3 - "$TMP" <<'PY'
import sys, pathlib
tmp = pathlib.Path(sys.argv[1])
src = (tmp.parent / "x") # unused
s = open("drivers/usb/hid_report.c").read()
old = """            case 0x8: { /* Report ID: a new report starts here */
                /* Persist the size of the report we are leaving. */
                if (cur_slot >= 0) r->report_bits[cur_slot] = bitpos;
                cur_id = (uint8_t)uval;
                r->has_report_id = 1;
                cur_slot = report_slot(r, cur_id);
                bitpos = (cur_slot >= 0) ? r->report_bits[cur_slot] : 0;
                break;
            }"""
new = """            case 0x8: { /* OLD BEHAVIOUR: one shared bit cursor */
                cur_id = (uint8_t)uval;
                r->has_report_id = 1;
                cur_slot = report_slot(r, cur_id);
                break;
            }"""
if old not in s:
    raise SystemExit("negative control is stale: the Report ID branch changed")
s = s.replace(old, new, 1)
(tmp / "hid_report_old.c").write_text(s)
PY

cc -std=c11 -Wall -Wextra -Werror -O1 -I "$TMP" -I "$ROOT" \
    tests/unit/test_usb_hid_report.c "$TMP/hid_report_old.c" -o "$TMP/t_old" || {
    echo "negcontrol: FAIL (old-shape parser did not compile)"; exit 1; }

set +e
OUT=$("$TMP/t_old" 2>&1)
RC=$?
set -e
echo "$OUT" | tail -8

FAILS=$(echo "$OUT" | grep -c "FAIL L" || true)
if [ "$RC" -eq 0 ] || [ "${FAILS:-0}" -lt 3 ]; then
    echo
    echo "negcontrol: FAIL — the host test passes against the pre-C5b parser"
    echo "            (rc=$RC, $FAILS failing checks). The test does not discriminate."
    exit 1
fi
echo
echo "negcontrol: PASS — the pre-C5b parser fails $FAILS check(s) in the same test"
