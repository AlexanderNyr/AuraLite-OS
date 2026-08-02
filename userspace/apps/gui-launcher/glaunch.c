/* glaunch — application launcher.  Lists installed GUI apps and spawns the
 * selected one in a new process. */
#include "auragui.h"
#include "unistd.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[24];
static ag_view_t view;

/* Programs are named, not pathed (FSLAYOUT_PLAN phase F2).
 *
 * This table used to hold "/gcalc", "/gedit" and so on.  Every one of those
 * was a place the F3 move could break something silently — the launcher has
 * no test that clicks its buttons, so a stale path here would produce a
 * button that does nothing and no failing test anywhere.
 *
 * The names are resolved through libc's search path at launch time, so the
 * launcher keeps working wherever the programs end up. */
struct app { const char *name; const char *prog; };
static struct app apps[] = {
    { "Calculator",    "gcalc"   },
    { "Text Editor",   "gedit"   },
    { "File Manager",  "gfiles"  },
    { "Terminal",      "gterm"   },
    { "System Monitor","gsysmon" },
    { "Task Manager",  "gtaskmgr"},
    { "Music Player",  "gaudio"  },
    { "Web Browser",   "gbrowser"},
    { "USB Manager",   "gusb"    },
    { "About",         "gabout"  },
    { "OpenGL Cube",   "glcube"  },
    { "OpenGL Gears",  "glgears" },
};

static void on_launch(ag_widget_t *w, void *u) {
    (void)w;
    const char *prog = (const char *)u;
    char path[128];
    if (prog_resolve(prog, path, (int)sizeof(path))) {
        spawn(path);
    }
}

int main(void) {
    int n = sizeof(apps) / sizeof(apps[0]);
    int H = 80 + n * 38;
    wid = ag_window_create(40, 60, 280, H, "Application Launcher", AG_WIN_DEFAULT & ~AG_WIN_RESIZABLE);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 24, AG_PANEL);

    ag_add_label(&view, 16, 16, "AuraLite Applications", AG_ACCENT);
    ag_add_label(&view, 16, 36, "Click to launch:", AG_DARK);

    for (int i = 0; i < n; i++) {
        ag_widget_t *b = ag_add_button(&view, 16, 60 + i * 38, 248, 32,
                                       apps[i].name, on_launch, (void *)apps[i].prog);
        b->bg = (i % 2 == 0) ? AG_ACCENT : 0x004080A0;
    }

    ag_view_run(&view, 0, 0);
    return 0;
}
