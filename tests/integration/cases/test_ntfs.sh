#!/usr/bin/env bash
# test_ntfs.sh — F7 integration wrapper (FSFULL_PLAN.md F7).
#
# Runs the F5b NTFS harness (tests/ntfs/test_ntfs.sh) so it is covered by
# the CI "fsfull" shard.  The harness is fully self-contained: it builds
# its own disks (each case owns its state — no shared disk between cases),
# drives its own QEMU boots, and exits nonzero if any of its assertions
# fail.  `exec` preserves that exit code.
#
# It was already exercised as a standalone target (patches/FS_F5b_ntfs.patch);
# this file only registers it with the shard runner.
set -u
exec bash "$(dirname "$0")/../../ntfs/test_ntfs.sh"
