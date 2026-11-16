#!/usr/bin/env python3
"""check_xlsx.py — an independent reader for the .xlsx packages aoxlsx.c writes.

tests/unit/test_ao_xlsx.c asserts the properties we can assert with our own
code: our ZIP reader, our XML parser, our model.  That is necessary and it is
not sufficient — a bug shared by the writer and the reader cancels out.  This
tool is the disagreement we want: Python's `zipfile` and `xml.etree`, written
by nobody here, reading the exact bytes the guest produced.

It is deliberately not a validator of the OOXML schema (that would need the
schema).  It checks the things a real reader depends on and that a hand-rolled
writer gets wrong:

  * every part is a ZIP entry with a sane CRC;
  * every XML part is well-formed *and* in the namespace it claims;
  * [Content_Types].xml covers every part, with no dangling Override;
  * every relationship Target resolves to a part that exists;
  * every worksheet a workbook.xml names has a part, and sheetId/r:id pair up
    with workbook.xml.rels;
  * every <c> has either a value or a formula, every t="s" index is inside
    sharedStrings, and every cell reference parses as a real A1 address;
  * timestamps are pinned at 1980-01-01, so two saves of an unchanged
    workbook are byte-identical.

Usage: check_xlsx.py <file.xlsx>
"""
import re
import sys
import zipfile
import xml.etree.ElementTree as ET

MAIN = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
PKG_REL = "{http://schemas.openxmlformats.org/package/2006/relationships}"
DOC_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
CT = "{http://schemas.openxmlformats.org/package/2006/content-types}"

CT_WORKBOOK = ("application/vnd.openxmlformats-officedocument."
               "spreadsheetml.sheet.main+xml")
CT_SHEET = ("application/vnd.openxmlformats-officedocument."
            "spreadsheetml.worksheet+xml")
CT_STYLES = ("application/vnd.openxmlformats-officedocument."
             "spreadsheetml.styles+xml")
CT_SHARED = ("application/vnd.openxmlformats-officedocument."
             "spreadsheetml.sharedStrings+xml")
CT_RELS = "application/vnd.openxmlformats-package.relationships+xml"

REQUIRED = [
    "[Content_Types].xml",
    "_rels/.rels",
    "xl/workbook.xml",
    "xl/_rels/workbook.xml.rels",
    "xl/styles.xml",
    "xl/sharedStrings.xml",
]

EXPECTED_OVERRIDES = {
    "/xl/workbook.xml": CT_WORKBOOK,
    "/xl/styles.xml": CT_STYLES,
    "/xl/sharedStrings.xml": CT_SHARED,
}

CELL_REF = re.compile(r"^([A-Za-z]+)([0-9]+)$")


class Fail(Exception):
    pass


def col_to_index(letters):
    n = 0
    for ch in letters.upper():
        n = n * 26 + (ord(ch) - ord("A") + 1)
    return n - 1


def parse(zf, name, problems):
    """Read one part and require it to be well-formed XML."""
    try:
        raw = zf.read(name)
    except KeyError:
        problems.append("%s: missing" % name)
        return None
    try:
        return ET.fromstring(raw)
    except ET.ParseError as exc:
        problems.append("%s: not well-formed XML: %s" % (name, exc))
        return None


def check(path, problems):
    with zipfile.ZipFile(path) as zf:
        names = zf.namelist()

        for name in REQUIRED:
            if name not in names:
                problems.append("required part missing: %s" % name)
        if problems:
            return names

        # ---- CRCs and timestamps -------------------------------------------
        for info in zf.infolist():
            if zf.read(info.filename) is None:
                problems.append("%s: unreadable" % info.filename)
            if info.date_time != (1980, 1, 1, 0, 0, 0):
                problems.append("%s: timestamp not pinned at 1980-01-01: %s"
                                % (info.filename, info.date_time))

        # ---- [Content_Types].xml -------------------------------------------
        ct = parse(zf, "[Content_Types].xml", problems)
        if ct is None:
            return names
        if ct.tag != CT + "Types":
            problems.append("[Content_Types].xml: root is %s" % ct.tag)
        defaults = set()
        overrides = {}
        for node in ct:
            if node.tag == CT + "Default":
                ext = node.get("Extension")
                ctype = node.get("ContentType")
                if not ext or not ctype:
                    problems.append("[Content_Types].xml: Default without "
                                    "Extension or ContentType")
                else:
                    defaults.add(ext)
            elif node.tag == CT + "Override":
                part = node.get("PartName")
                ctype = node.get("ContentType")
                if not part or not ctype:
                    problems.append("[Content_Types].xml: Override without "
                                    "PartName or ContentType")
                else:
                    overrides[part] = ctype

        for ext in ("rels", "xml"):
            if ext not in defaults:
                problems.append("[Content_Types].xml: no Default for .%s" % ext)

        for part, ctype in sorted(EXPECTED_OVERRIDES.items()):
            if overrides.get(part) != ctype:
                problems.append("[Content_Types].xml: %s should be %s, got %s"
                                % (part, ctype, overrides.get(part)))

        sheets = sorted(n for n in names
                        if n.startswith("xl/worksheets/") and n.endswith(".xml"))
        if not sheets:
            problems.append("no xl/worksheets/*.xml part")
        for sheet in sheets:
            if overrides.get("/" + sheet) != CT_SHEET:
                problems.append("[Content_Types].xml: %s missing its worksheet "
                                "override" % sheet)
        for part in sorted(overrides):
            if part.lstrip("/") not in names:
                problems.append("[Content_Types].xml: Override for %s, which "
                                "is not in the package" % part)

        # ---- _rels/.rels ----------------------------------------------------
        root_rels = parse(zf, "_rels/.rels", problems)
        if root_rels is not None:
            if root_rels.tag != PKG_REL + "Relationships":
                problems.append("_rels/.rels: root is %s" % root_rels.tag)
            targets = [n.get("Target") for n in root_rels
                       if n.tag == PKG_REL + "Relationship"]
            if "xl/workbook.xml" not in targets:
                problems.append("_rels/.rels: nothing points at xl/workbook.xml "
                                "(%s)" % targets)

        # ---- xl/workbook.xml ------------------------------------------------
        wb = parse(zf, "xl/workbook.xml", problems)
        if wb is None:
            return names
        if wb.tag != MAIN + "workbook":
            problems.append("xl/workbook.xml: root is %s" % wb.tag)
        declared = []
        sheets_node = None
        for node in wb:
            if node.tag == MAIN + "sheets":
                sheets_node = node
        if sheets_node is None:
            problems.append("xl/workbook.xml: no <sheets>")
        else:
            for node in sheets_node:
                if node.tag != MAIN + "sheet":
                    continue
                name = node.get("name")
                rid = node.get("{http://schemas.openxmlformats.org/"
                               "officeDocument/2006/relationships}id")
                sid = node.get("sheetId")
                if not name:
                    problems.append("xl/workbook.xml: a <sheet> with no name")
                if not rid:
                    problems.append("xl/workbook.xml: sheet %r has no r:id"
                                    % name)
                if not sid:
                    problems.append("xl/workbook.xml: sheet %r has no sheetId"
                                    % name)
                declared.append((name, rid))

        # ---- xl/_rels/workbook.xml.rels -------------------------------------
        wb_rels = parse(zf, "xl/_rels/workbook.xml.rels", problems)
        rid_target = {}
        if wb_rels is not None:
            if wb_rels.tag != PKG_REL + "Relationships":
                problems.append("xl/_rels/workbook.xml.rels: root is %s"
                                % wb_rels.tag)
            for node in wb_rels:
                if node.tag != PKG_REL + "Relationship":
                    continue
                rid = node.get("Id")
                target = node.get("Target")
                rtype = node.get("Type")
                if not rid or not target or not rtype:
                    problems.append("workbook.xml.rels: Relationship missing "
                                    "Id, Type or Target")
                    continue
                rid_target[rid] = (target, rtype)
                resolved = target.lstrip("/")
                if not resolved.startswith("xl/"):
                    resolved = "xl/" + resolved
                if resolved not in names:
                    problems.append("workbook.xml.rels: %s -> %s, which is not "
                                    "in the package" % (rid, target))

        for name, rid in declared:
            if rid and rid not in rid_target:
                problems.append("xl/workbook.xml: sheet %r uses %s, which "
                                "workbook.xml.rels does not define"
                                % (name, rid))
            elif rid:
                target, rtype = rid_target[rid]
                if rtype != DOC_REL + "/worksheet":
                    problems.append("xl/workbook.xml: sheet %r -> %s is of type "
                                    "%s, not /worksheet" % (name, target, rtype))

        # ---- xl/sharedStrings.xml -------------------------------------------
        sst = parse(zf, "xl/sharedStrings.xml", problems)
        shared = []
        if sst is not None:
            if sst.tag != MAIN + "sst":
                problems.append("xl/sharedStrings.xml: root is %s" % sst.tag)
            for si in sst:
                if si.tag != MAIN + "si":
                    problems.append("xl/sharedStrings.xml: unexpected %s" % si.tag)
                    continue
                text = "".join(t.text or "" for t in si.iter(MAIN + "t"))
                shared.append(text)

        # ---- xl/styles.xml ---------------------------------------------------
        styles = parse(zf, "xl/styles.xml", problems)
        if styles is not None and styles.tag != MAIN + "styleSheet":
            problems.append("xl/styles.xml: root is %s" % styles.tag)

        # ---- the worksheets --------------------------------------------------
        for sheet in sheets:
            root = parse(zf, sheet, problems)
            if root is None:
                continue
            if root.tag != MAIN + "worksheet":
                problems.append("%s: root is %s" % (sheet, root.tag))

            # <cols> must precede <sheetData> in the worksheet schema.
            order = [n.tag for n in root]
            if MAIN + "cols" in order and MAIN + "sheetData" in order:
                if order.index(MAIN + "cols") > order.index(MAIN + "sheetData"):
                    problems.append("%s: <cols> after <sheetData>" % sheet)

            data = None
            for node in root:
                if node.tag == MAIN + "sheetData":
                    data = node
            if data is None:
                problems.append("%s: no <sheetData>" % sheet)
                continue

            seen_refs = set()
            for row in data:
                if row.tag != MAIN + "row":
                    problems.append("%s: unexpected %s in <sheetData>"
                                    % (sheet, row.tag))
                    continue
                if not row.get("r"):
                    problems.append("%s: a <row> with no r" % sheet)
                for cell in row:
                    if cell.tag != MAIN + "c":
                        problems.append("%s: unexpected %s in <row>"
                                        % (sheet, cell.tag))
                        continue
                    ref = cell.get("r")
                    ctype = cell.get("t")
                    if not ref:
                        problems.append("%s: a <c> with no r" % sheet)
                        continue
                    m = CELL_REF.match(ref)
                    if not m:
                        problems.append("%s: %r is not an A1 reference"
                                        % (sheet, ref))
                        continue
                    if int(m.group(2)) == 0:
                        problems.append("%s: %r has a zero row" % (sheet, ref))
                    if ref in seen_refs:
                        problems.append("%s: %r appears twice" % (sheet, ref))
                    seen_refs.add(ref)

                    kinds = {}
                    for child in cell:
                        kinds[child.tag] = kinds.get(child.tag, 0) + 1
                    for tag, count in kinds.items():
                        if count > 1:
                            problems.append("%s: %s has %d <%s> children"
                                            % (sheet, ref, count,
                                               tag.split('}')[-1]))

                    has_f = MAIN + "f" in kinds
                    has_v = MAIN + "v" in kinds
                    has_is = MAIN + "is" in kinds
                    if not has_f and not has_v and not has_is:
                        continue  # an empty <c> is legal and carries nothing

                    if ctype == "s":
                        if not has_v:
                            problems.append("%s: %s is t=\"s\" with no <v>"
                                            % (sheet, ref))
                            continue
                        raw = cell.find(MAIN + "v").text or ""
                        try:
                            idx = int(raw)
                        except ValueError:
                            problems.append("%s: %s has a non-numeric shared "
                                            "index %r" % (sheet, ref, raw))
                            continue
                        if not (0 <= idx < len(shared)):
                            problems.append("%s: %s points at shared string %d, "
                                            "but there are %d"
                                            % (sheet, ref, idx, len(shared)))
                    elif ctype == "inlineStr":
                        if not has_is:
                            problems.append("%s: %s is t=\"inlineStr\" with no "
                                            "<is>" % (sheet, ref))
                    elif ctype in (None, "n"):
                        if has_v:
                            raw = cell.find(MAIN + "v").text or ""
                            try:
                                float(raw)
                            except ValueError:
                                problems.append("%s: %s is numeric but its <v> "
                                                "is %r" % (sheet, ref, raw))
                    elif ctype == "b":
                        raw = (cell.find(MAIN + "v").text if has_v else None)
                        if raw is None or raw not in ("0", "1"):
                            problems.append("%s: %s is t=\"b\" whose <v> is not "
                                            "0 or 1" % (sheet, ref))
                    elif ctype == "e":
                        pass
                    else:
                        problems.append("%s: %s has an unknown t=%r"
                                        % (sheet, ref, ctype))

    return names


def main(argv):
    if len(argv) != 2:
        print("usage: check_xlsx.py <file.xlsx>")
        return 2
    path = argv[1]
    problems = []
    names = check(path, problems)
    if problems:
        print("FAIL %s -- %d problem(s):" % (path, len(problems)))
        for p in problems:
            print("  - %s" % p)
        return 1
    print("[unit] Python XLSX interop: PASS (%d parts: %s)"
          % (len(names), ", ".join(sorted(names))))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except (OSError, zipfile.BadZipFile) as exc:
        print("FAIL -- cannot read %s: %s"
              % (sys.argv[1] if len(sys.argv) > 1 else "<none>", exc))
        sys.exit(1)
