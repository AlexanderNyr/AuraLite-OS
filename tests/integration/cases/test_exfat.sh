#!/usr/bin/env bash
# test_exfat.sh — F7 integration wrapper (FSFULL_PLAN.md F7).
#
# Runs the F5 exFAT harness (tests/exfat/test_exfat.sh) so it is covered
# by the CI "fsfull" shard.  The harness is fully self-contained: it builds
# its own disks (each case owns its state — no shared disk between cases),
# drives its own QEMU boots, and exits nonzero if any of its assertions
# fail.  `exec` preserves that exit code.
#
# It was already exercised as a standalone target (patches/FS_F5_exfat.patch);
# this file only registers it with the shard runner.
set -u
exec bash "$(dirname "$0")/../../exfat/test_exfat.sh"
