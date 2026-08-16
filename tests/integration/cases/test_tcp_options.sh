#!/usr/bin/env bash
#
# test_tcp_options.sh — MATURITY_PLAN.md M6c.
#
# Until this phase tcp_send_segment_at() hardcoded data_offset = 5<<4, so
# the stack had never put a single option on the wire: it could not
# advertise its MSS and could not ask for SACK.  Nothing parsed the peer's
# options either.
#
# The gate proves the negotiation actually happens against a real peer
# rather than in a unit test: the guest must report what the peer offered.
#
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/lib.sh
. "$SCRIPT_DIR/../lib/lib.sh"

il_init "tcp_options"
il_section "TCP option negotiation (M6c: MSS + SACK-permitted)"

LOG="$IL_LOGDIR/tcp_options.log"

il_send_delay 9
il_send "http example.com /"
il_send_delay 12
il_send "exit"

il_run_qemu "$LOG" 75 \
    -drive file="$IL_BUILD/auralite.iso",format=raw,if=ide,snapshot=on \
    -boot order=c -m 512M -smp 2 -display none -no-reboot -cpu qemu64 \
    -netdev user,id=n0 -device e1000,netdev=n0

# WE put options on the wire.  This is the assertion the phase turns on:
# before M6c the SYN was a bare 20-byte header and this line cannot appear.
# The byte counts are derived from the header actually built, so a SYN that
# loses its options fails here even if the peer still answers.
il_assert_grep_fixed "$LOG" "[tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm" \
    "our SYN carries MSS + SACK-permitted (28-byte header)"

# The peer's option block was parsed at all.  SLIRP always offers an MSS,
# so this line appearing at all is the proof the parser ran on real bytes.
il_assert_grep "$LOG" "\[tcp\] peer options: mss=[0-9]+" \
    "the peer's TCP options were parsed"

# And the MSS is a sane negotiated value, not a parse artefact.  SLIRP
# offers 1460; anything absurd here means the option walk is misaligned.
il_assert_grep "$LOG" "\[tcp\] peer options: mss=(1460|1440|1400|536)" \
    "negotiated MSS is a plausible value"

# The handshake still completes with options on the SYN -- the regression
# risk of this phase is a peer rejecting our now-larger header.
il_assert_grep_fixed "$LOG" "ESTABLISHED" \
    "handshake still completes with options on the SYN"

il_assert_no_grep "$LOG" "PANIC|page fault in kernel" "no kernel fault"

il_summary
