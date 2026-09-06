# tools/gdb/aura.gdb -- AuraLite kernel debugging conveniences (RESIDUE2 T9).
#
# Usage:
#   gdb -ex 'source tools/gdb/aura.gdb' build/kernel.elf
#   (gdb) aura_theme            # the ACTIVE desktop theme, colors as #RRGGBB
#   (gdb) aura_default_theme    # the built-in default (const, file-backed)
#   (gdb) aura_windows          # live window table: wid, owner, rect, title
#   (gdb) target remote :1234   # attach to QEMU started with -s -S
#
# The pretty-printers (tools/gdb/pretty.py) trigger automatically on
# `print` of any gui_theme_t / struct tcb / struct ofd value.

source -s tools/gdb/pretty.py

python
import gdb


class AuraTheme(gdb.Command):
    """Print the ACTIVE desktop theme (gui.c's active_theme)."""
    def __init__(self):
        super(AuraTheme, self).__init__("aura_theme", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print(gdb.parse_and_eval("active_theme"))


class AuraDefaultTheme(gdb.Command):
    """Print the built-in default theme (file-backed const: no target needed)."""
    def __init__(self):
        super(AuraDefaultTheme, self).__init__("aura_default_theme",
                                                  gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        print(gdb.parse_and_eval("default_theme"))


class AuraWindows(gdb.Command):
    """Dump the GUI window table: wid, owner pid, rect, flags, title."""
    def __init__(self):
        super(AuraWindows, self).__init__("aura_windows", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        try:
            wins = gdb.parse_and_eval("windows")
            n = int(gdb.parse_and_eval("GUI_MAX_WINDOWS"))
        except gdb.error as e:
            print("aura: %s (attach to a live target?)" % e)
            return
        live = 0
        for i in range(min(n, 64)):
            w = wins[i]
            try:
                if int(w["in_use"]) == 0:
                    continue
            except gdb.error:
                break
            live += 1
            print("wid %2d owner %-4d rect %4d,%4d %3ux%-3u fl %08x '%s'" % (
                i, int(w["owner_pid"]), int(w["x"]), int(w["y"]),
                int(w["w"]), int(w["h"]), int(w["flags"]),
                w["title"].string()))
        print("%d live window(s)" % live)


AuraTheme()
AuraDefaultTheme()
AuraWindows()
end
