"""tools/gdb/pretty.py -- GDB pretty-printers for AuraLite kernel structures.

RESIDUE2 T9 (the GDB helper-scripts box).  Loaded by aura.gdb:

    gdb -ex 'source tools/gdb/aura.gdb' build/kernel.elf

Printers are matched by SIGNATURE (field names a type carries), not by
type name alone: several kernel structs are anonymous or carry arch
prefixes, and a printer that matches "has these fields" keeps working
when a field is appended.  Everything degrades to the raw display when
the value's type does not match -- printers must never hide data.

Colors print as #RRGGBB (the kernel's 0x00RRGGBB convention), thread
states as mnemonics (THREAD_READY &c.), and the theme/tcb get one-line
summaries with the fields a debugging session actually wants first.
"""

import gdb  # provided by the gdb embedded interpreter


# ---- helpers -------------------------------------------------------------

def _u64(val):
    return int(val) & 0xFFFFFFFFFFFFFFFF


def _color(word):
    v = int(word) & 0xFFFFFFFF
    return "#%06X" % (v & 0xFFFFFF)


class ThemePrinter:
    """gui_theme_t: accent colors first, geometry second."""

    HEAD = ("desktop_top", "win_bg", "title_active", "taskbar_h")

    def __init__(self, val):
        self.val = val

    def to_string(self):
        t = self.val
        parts = []
        for f in ("desktop_top", "title_active", "title_inactive",
                  "win_bg", "taskbar_bg", "start_btn_bg"):
            try:
                parts.append("%s=%s" % (f, _color(t[f])))
            except gdb.error:
                pass
        geom = []
        for f in ("taskbar_h", "titlebar_h", "border_w"):
            try:
                geom.append("%s=%d" % (f, int(t[f])))
            except gdb.error:
                pass
        return "gui_theme {" + ", ".join(parts + geom) + "}"


_THREAD_STATES = {
    0: "READY", 1: "RUNNING", 2: "BLOCKED", 3: "STOPPED",
    4: "SLEEPING", 5: "DEAD",
}


class TcbPrinter:
    """struct tcb: who it is, where it is, what it waits for."""

    def __init__(self, val):
        self.val = val

    def _state(self, v):
        s = _THREAD_STATES.get(int(v) & 0x7FFFFFFF)
        return "%d (%s)" % (int(v), s) if s else str(int(v))

    def to_string(self):
        t = self.val
        try:
            name = t["name"].string()
        except gdb.error:
            name = "?"
        state = self._state(t["state"])
        return "tcb id=%d '%s' state=%s rsp=0x%x" % (
            int(t["id"]), name, state, _u64(t["rsp"]))


class OfdPrinter:
    """struct ofd (open file description): path + pos + refs."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        o = self.val
        out = ["ofd refcnt=%d pos=%d" % (int(o["refcnt"]), int(o["pos"]))]
        try:
            out.append("path='%s'" % o["path"].string())
        except gdb.error:
            pass
        return " ".join(out)


# ---- registration ---------------------------------------------------------

def _has_fields(val, names):
    for n in names:
        try:
            val[n]
        except gdb.error:
            return False
    return True


def aura_lookup(val):
    if _has_fields(val, ThemePrinter.HEAD):
        return ThemePrinter(val)
    if _has_fields(val, ("id", "name", "state", "rsp")):
        return TcbPrinter(val)
    if str(val.type).startswith("struct ofd") or _has_fields(
            val, ("refcnt", "pos", "path")):
        return OfdPrinter(val)
    return None


class _AuraCollection:
    """Plain-callable printer collection.

    Debian's gdb ships without the gdb.printing helper module, so the
    kit registers the lowest-common-denominator way: any callable in
    gdb.pretty_printers is consulted for every value; returning None
    falls through to the default display.  `name` makes re-sourcing
    idempotent and `info pretty-printer` readable.
    """

    name = "aura"
    enabled = True

    def __call__(self, val):
        return aura_lookup(val)


def load():
    # replace-on-reload so re-sourcing is idempotent
    for old in list(gdb.pretty_printers):
        if getattr(old, "name", "") == "aura":
            gdb.pretty_printers.remove(old)
    try:
        pp = gdb.printing.RegexpCollectionPrettyPrinter("aura")
        pp.add_printer("aura", ".*", aura_lookup)
        gdb.pretty_printers.append(pp)
    except AttributeError:                      # no gdb.printing module
        gdb.pretty_printers.append(_AuraCollection())


load()
