#!/usr/bin/env bash
# check_provenance.sh — WIN32_PLAN.md phase W32-0 gate.
#
# Enforces the licensing rules in w32/LICENSING.md mechanically, because a rule
# that is only written down is a rule that decays:
#
#   1. every source file under w32/ is listed in w32/PROVENANCE.md;
#   2. every source file carries a licence/provenance comment header;
#   3. no file under w32/ mentions Wine or ReactOS as a source (the two
#      licence-incompatible implementations contributors reach for);
#   4. no Microsoft binary has been committed.
#
# Usage:
#   tools/check_provenance.sh            # check the tree
#   tools/check_provenance.sh --selftest # prove the check actually fails
#
# The --selftest mode is the negative control: it plants a violation in a
# temporary copy and asserts the checker rejects it.  Without that, a checker
# that silently passes everything looks identical to a clean tree.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W32="$ROOT/w32"
PROV="$W32/PROVENANCE.md"

fail=0
note() { echo "  $*"; }
bad()  { echo "  FAIL: $*"; fail=1; }

check_tree() {
    local root="$1" w32="$1/w32" prov="$1/w32/PROVENANCE.md"

    if [ ! -d "$w32" ]; then
        echo "[provenance] SKIP: no w32/ directory"
        return 0
    fi
    if [ ! -f "$prov" ]; then
        bad "w32/PROVENANCE.md is missing"
        return 1
    fi

    # 1 + 2: every source file is recorded and self-describing.
    while IFS= read -r f; do
        rel="${f#$w32/}"
        if ! grep -qF -- "$rel" "$prov"; then
            bad "$rel is not listed in w32/PROVENANCE.md"
        fi
        # A licence/provenance header must appear near the top.  Vendored
        # files carry their upstream notice; ours name the spec they follow.
        if ! head -n 25 "$f" | grep -qiE \
             "WIN32_PLAN|public domain|Apache|ZPL|BSD|MIT|Copyright"; then
            bad "$rel has no licence/provenance header in its first 25 lines"
        fi
    done < <(find "$w32" -type f \( -name '*.c' -o -name '*.h' \) | sort)

    # 3: the two forbidden implementations must not appear as sources.  They
    # are named in LICENSING.md and PROVENANCE.md *as prohibitions*, which is
    # why those two files are exempt.
    while IFS= read -r f; do
        case "$(basename "$f")" in
            LICENSING.md|PROVENANCE.md) continue ;;
        esac
        if grep -qiE '\b(wine|reactos)\b' "$f"; then
            bad "$(basename "$f") mentions Wine/ReactOS; see w32/LICENSING.md"
        fi
    done < <(find "$w32" -type f | sort)

    # 4: no Microsoft binaries anywhere in the tree.
    while IFS= read -r f; do
        bad "binary that must not be redistributed: ${f#$root/}"
    done < <(find "$root" -type f \
                  \( -iname '*.dll' -o -iname '*.sys' -o -iname '*.msi' \) \
                  -not -path '*/.git/*' -not -path '*/build/*' | sort)

    return 0
}

if [ "${1:-}" = "--selftest" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/w32/src"
    cp "$PROV" "$tmp/w32/PROVENANCE.md"
    cp "$W32/LICENSING.md" "$tmp/w32/LICENSING.md"

    # Violation: a source file with no provenance entry and no licence header.
    printf 'int sneaky(void) { return 0; }\n' > "$tmp/w32/src/sneaky.c"

    echo "[provenance] self-test: planting an unrecorded file..."
    # Note: check_tree runs in a command substitution (a subshell), so its
    # assignment to $fail is not visible here.  Judge it by its output, which
    # is the only signal that actually crosses the subshell boundary.
    out="$(check_tree "$tmp" 2>&1)"
    if ! echo "$out" | grep -q "FAIL:"; then
        echo "$out"
        echo "[provenance] SELF-TEST FAILED: checker accepted an unrecorded file"
        exit 1
    fi
    echo "$out" | sed 's/^/    /'
    echo "[provenance] self-test PASS: violation was detected as required"
    exit 0
fi

echo "[provenance] checking w32/ ..."
check_tree "$ROOT"

if [ "$fail" -ne 0 ]; then
    echo "[provenance] FAIL — see w32/LICENSING.md"
    exit 1
fi
count=$(find "$W32" -type f \( -name '*.c' -o -name '*.h' \) | wc -l)
echo "[provenance] PASS: $count source file(s) recorded, no forbidden sources"
