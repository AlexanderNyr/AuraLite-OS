#!/usr/bin/env python3
"""Build the WR-2 byte-known extract fixture from in-tree sources.

The WR-2 live gate (tests/integration/cases/test_wr2_7zip_live.sh) points the
pinned 7-Zip File Manager at a committed archive and asserts the extracted
bytes match a known plaintext.  Per W32RUN_PLAN.md §WR-2 and the no-foreign-
bytes rule (w32/LICENSING.md), the archive is *built in CI from in-tree
sources*, never committed as opaque bytes.

Inputs  : tests/fixtures/wr2/content/*        (the known plaintext)
Outputs : build/wr2/wr2_fixture.zip           (STORED, deterministic)
          build/wr2/wr2_fixture.manifest.txt  (per-member content sha256)

A ZIP (not 7z) is used deliberately: it is buildable with the Python stdlib
alone (no external archiver, so the builder itself pulls in no foreign bytes),
and the pinned 7zFM.exe opens .zip natively.  Extraction reproduces each
member's bytes regardless of the container, so the per-member sha256 below is
what the gate checks.

Usage: tools/wr2_make_fixture.py [--check]
  (no args) build the archive + manifest
  --check   build, then verify each member round-trips to its source sha256
"""

import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "tests", "fixtures", "wr2", "content")
OUT_DIR = os.path.join(ROOT, "build", "wr2")
ZIP_PATH = os.path.join(OUT_DIR, "wr2_fixture.zip")
MANIFEST = os.path.join(OUT_DIR, "wr2_fixture.manifest.txt")

# A fixed DOS timestamp so the container bytes are reproducible run to run.
FIXED_DT = (1980, 1, 1, 0, 0, 0)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def members():
    """(arcname, bytes) for each source file, sorted for determinism."""
    out = []
    for name in sorted(os.listdir(SRC_DIR)):
        path = os.path.join(SRC_DIR, name)
        if os.path.isfile(path):
            with open(path, "rb") as handle:
                out.append((name, handle.read()))
    return out


def build():
    os.makedirs(OUT_DIR, exist_ok=True)
    entries = members()
    if not entries:
        print("wr2_make_fixture: no sources in %s" % SRC_DIR, file=sys.stderr)
        return 1
    # Deterministic archive: STORED, fixed timestamps, sorted order.
    with zipfile.ZipFile(ZIP_PATH, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, data in entries:
            info = zipfile.ZipInfo(filename=name, date_time=FIXED_DT)
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o600 << 16
            zf.writestr(info, data)
    lines = []
    for name, data in entries:
        lines.append("%s  %s" % (sha256(data), name))
    with open(MANIFEST, "w", encoding="utf-8") as handle:
        handle.write("# WR-2 fixture: sha256 of each member's plaintext\n")
        handle.write("\n".join(lines) + "\n")
        handle.write("# archive: %s  %s\n"
                     % (sha256(open(ZIP_PATH, "rb").read()),
                        os.path.basename(ZIP_PATH)))
    print("wr2_make_fixture: wrote %s (%d members)" % (ZIP_PATH, len(entries)))
    for line in lines:
        print("  " + line)
    return 0


def check():
    rc = build()
    if rc:
        return rc
    # Round-trip: re-open the archive and confirm each member's sha matches.
    src = {n: sha256(d) for n, d in members()}
    with zipfile.ZipFile(ZIP_PATH, "r") as zf:
        for name in zf.namelist():
            got = sha256(zf.read(name))
            want = src.get(name)
            if got != want:
                print("wr2_make_fixture: CHECK FAIL %s %s != %s"
                      % (name, got, want), file=sys.stderr)
                return 1
    print("wr2_make_fixture: CHECK OK (%d members round-trip byte-exact)"
          % len(src))
    return 0


if __name__ == "__main__":
    sys.exit(check() if "--check" in sys.argv[1:] else build())
