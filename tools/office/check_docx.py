#!/usr/bin/env python3
"""check_docx.py — validate a .docx our C writer produced, with Python's own
readers (OFFICE_PLAN.md OF8).

    python3 tools/office/check_docx.py build/test_ao_ooxml_docx.docx

Why this exists when tests/unit/test_ao_ooxml.c already passes: that test
checks our archive against OUR reader.  A parser agreeing with itself proves
nothing about whether Word, LibreOffice or a browser can open the file.  This
tool is the independent half of the receipt -- zipfile and xml.etree are not
code we wrote, so when they accept the package the claim "it is a .docx" is
somebody else's opinion, not just ours.

It checks the parts a validator looks for, that every XML part is well formed,
that the content-type overrides and relationship targets line up with the parts
actually present, and that no part is both undeclared and unreferenced.  It
does NOT check that Word renders it correctly -- that is a live pass with a
real suite, and no amount of Python substitutes for it.

Exit status 0 on success, 1 on any failure.  Every failure names the part and
the rule, because "the package is invalid" is not a diagnosis.
"""

import sys
import zipfile
import xml.etree.ElementTree as ET

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
PKG_REL = "{http://schemas.openxmlformats.org/package/2006/relationships}"
CT = "{http://schemas.openxmlformats.org/package/2006/content-types}"

DOCUMENT_CT = ("application/vnd.openxmlformats-officedocument."
               "wordprocessingml.document.main+xml")

# Parts that must exist for the package to open at all.
REQUIRED = [
    "[Content_Types].xml",
    "_rels/.rels",
    "word/document.xml",
    "word/_rels/document.xml.rels",
]

# Overrides we emit for non-default extensions.
EXPECTED_OVERRIDES = {
    "/word/document.xml": DOCUMENT_CT,
    "/word/styles.xml": ("application/vnd.openxmlformats-officedocument."
                         "wordprocessingml.styles+xml"),
    "/word/numbering.xml": ("application/vnd.openxmlformats-officedocument."
                            "wordprocessingml.numbering+xml"),
}


class Fail(Exception):
    pass


def parse(zf, name, problems):
    """Read and parse one part; a parse error is a problem, not a crash."""
    try:
        data = zf.read(name)
    except KeyError:
        problems.append("missing part: %s" % name)
        return None
    try:
        return ET.fromstring(data)
    except ET.ParseError as exc:
        problems.append("%s is not well-formed XML: %s" % (name, exc))
        return None


def main(argv):
    if len(argv) != 2:
        print("usage: check_docx.py <file.docx>", file=sys.stderr)
        return 2
    path = argv[1]
    problems = []

    if not zipfile.is_zipfile(path):
        print("FAIL %s is not a ZIP archive" % path)
        return 1

    with zipfile.ZipFile(path) as zf:
        # 1. every stored part reads back with a correct CRC
        bad = zf.testzip()
        if bad is not None:
            problems.append("CRC/size mismatch in part: %s" % bad)

        names = zf.namelist()

        # 2. the parts a validator requires
        for part in REQUIRED:
            if part not in names:
                problems.append("required part absent: %s" % part)
        if problems:
            raise Fail(problems)

        # 3. every XML part is well formed
        for name in names:
            if name.endswith(".xml") or name.endswith(".rels"):
                parse(zf, name, problems)

        # 4. [Content_Types].xml declares each non-default part
        ct = parse(zf, "[Content_Types].xml", problems)
        if ct is not None:
            defaults = {d.get("Extension", "").lower()
                        for d in ct.findall(CT + "Default")}
            overrides = {o.get("PartName"): o.get("ContentType")
                         for o in ct.findall(CT + "Override")}
            for part, want in EXPECTED_OVERRIDES.items():
                if overrides.get(part) != want:
                    problems.append(
                        "[Content_Types].xml: %s declares %r, expected %r"
                        % (part, overrides.get(part), want))
            if "png" not in defaults:
                problems.append("[Content_Types].xml: no Default for png")
            for name in names:
                if name.endswith("/") or name.startswith("[Content_Types]"):
                    continue
                ext = name.rsplit(".", 1)[-1].lower() if "." in name else ""
                if ext in defaults:
                    continue
                if "/" + name in overrides:
                    continue
                problems.append(
                    "[Content_Types].xml: no declaration for %s" % name)

        # 5. _rels/.rels reaches the document part, and declares the right type
        pkg = parse(zf, "_rels/.rels", problems)
        if pkg is not None:
            targets = {r.get("Target") for r in pkg.findall(PKG_REL + "Relationship")}
            if "word/document.xml" not in targets:
                problems.append("_rels/.rels does not target word/document.xml")

        # 6. every relationship in word/_rels/document.xml.rels resolves to a part
        #    that is actually in the archive -- a dangling rId is how a picture
        #    silently vanishes in a real suite.
        doc_rels = parse(zf, "word/_rels/document.xml.rels", problems)
        if doc_rels is not None:
            base = "word/"
            for rel in doc_rels.findall(PKG_REL + "Relationship"):
                target = rel.get("Target", "")
                if target.startswith("/"):
                    resolved = target.lstrip("/")
                else:
                    resolved = base + target
                if resolved not in names and target not in names:
                    problems.append(
                        "document.xml.rels: %s -> %s is not in the archive"
                        % (rel.get("Id"), target))

        # 7. the document part has a body and the markup we claim to write
        doc = parse(zf, "word/document.xml", problems)
        if doc is not None:
            if doc.find(W + "body") is None:
                problems.append("word/document.xml has no w:body")
            if doc.tag != W + "document":
                problems.append("word/document.xml root is %s, expected w:document"
                                % doc.tag)

        # 8. determinism is a property of the writer, but the timestamps are a
        #    property of the archive: everything pinned to 1980-01-01 means two
        #    saves of an unchanged document are byte-identical.
        for info in zf.infolist():
            if info.date_time != (1980, 1, 1, 0, 0, 0):
                problems.append("%s has a non-deterministic timestamp %r"
                                % (info.filename, info.date_time))

    if problems:
        print("FAIL %s -- %d problem(s):" % (path, len(problems)))
        for p in problems:
            print("  - %s" % p)
        return 1

    print("[unit] Python DOCX interop: PASS (%d parts: %s)"
          % (len(names), ", ".join(sorted(names))))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Fail as exc:
        print("FAIL -- %d problem(s):" % len(exc.args[0]))
        for p in exc.args[0]:
            print("  - %s" % p)
        sys.exit(1)
    except (OSError, zipfile.BadZipFile) as exc:
        print("FAIL -- cannot read %s: %s" % (sys.argv[1] if len(sys.argv) > 1
                                              else "<none>", exc))
        sys.exit(1)
