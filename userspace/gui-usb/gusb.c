/* gusb — USB manager GUI. */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"

static int wid;
static ag_widget_t widgets[20];
static ag_view_t view;
static ag_widget_t *status_box, *capacity_box, *fat_box, *magic_box, *detail_box;

static void set_text(ag_widget_t *w, const char *s) { ag_textbox_set(w, s ? s : ""); }
static void make2(char *out, const char *a, const char *b) {
    int p = 0;
    while (a && *a && p < 127) out[p++] = *a++;
    while (b && *b && p < 127) out[p++] = *b++;
    out[p] = 0;
}
static void make4(char *out, const char *a, const char *b, const char *c, const char *d) {
    int p = 0;
    const char *parts[4] = {a,b,c,d};
    for (int i = 0; i < 4; i++) {
        const char *s = parts[i];
        while (s && *s && p < 127) out[p++] = *s++;
    }
    out[p] = 0;
}

static int read_file(const char *path, char *buf, int max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t n = read(fd, buf, (size_t)(max - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int line_value(const char *info, const char *key, char *out, int max) {
    int klen = (int)strlen(key);
    const char *p = info;
    while (*p) {
        const char *line = p;
        int i = 0;
        while (line[i] && line[i] != '\n') i++;
        if (i >= klen && strncmp(line, key, (size_t)klen) == 0) {
            int n = i - klen;
            if (n >= max) n = max - 1;
            memcpy(out, line + klen, (size_t)n);
            out[n] = 0;
            return 0;
        }
        p = line + i;
        if (*p == '\n') p++;
    }
    return -1;
}

static char hex_digit(unsigned v) { return (char)(v < 10 ? '0' + v : 'A' + (v - 10)); }

static void refresh_usb(void) {
    char info[512];
    char tmp[128];
    int n = read_file("/usb/info", info, sizeof(info));
    if (n < 0) {
        set_text(status_box, "status: /usb unavailable");
        set_text(capacity_box, "capacity: unknown");
        set_text(fat_box, "fat32: unknown");
        set_text(magic_box, "sector0: unavailable");
        set_text(detail_box, "Open /usb in File Manager after a USB storage device is attached.");
        printf("[gusb] /usb unavailable\n");
        return;
    }

    if (line_value(info, "status: ", tmp, sizeof(tmp)) == 0) {
        char line[128];
        make2(line, "status: ", tmp);
        set_text(status_box, line);
    }
    char sectors[32] = "0", bytes[32] = "0";
    (void)line_value(info, "sectors: ", sectors, sizeof(sectors));
    (void)line_value(info, "bytes: ", bytes, sizeof(bytes));
    char capline[128];
    make4(capline, "capacity: ", sectors, " sectors / ", bytes);
    set_text(capacity_box, capline);
    if (line_value(info, "fat32: ", tmp, sizeof(tmp)) == 0) {
        char line[128];
        make2(line, "fat32: ", tmp);
        set_text(fat_box, line);
    } else {
        set_text(fat_box, "fat32: not reported");
    }

    int fd = open("/usb/sector0.bin", O_RDONLY);
    if (fd >= 0) {
        unsigned char sec[16];
        int64_t r = read(fd, sec, sizeof(sec));
        close(fd);
        if (r > 0) {
            char h[80];
            int p = 0;
            const char *prefix = "sector0: ";
            while (*prefix) h[p++] = *prefix++;
            for (int i = 0; i < r && i < 16; i++) {
                if (i) h[p++] = ' ';
                h[p++] = hex_digit(sec[i] >> 4);
                h[p++] = hex_digit(sec[i] & 0x0F);
            }
            h[p] = 0;
            set_text(magic_box, h);
        }
    } else {
        set_text(magic_box, "sector0: no media");
    }

    set_text(detail_box, info);
    printf("[gusb] usb info loaded\n");
}

static void on_refresh(ag_widget_t *w, void *u) { (void)w; (void)u; refresh_usb(); }
static void on_files(ag_widget_t *w, void *u) { (void)w; (void)u; spawn("/gfiles"); }
static void on_term(ag_widget_t *w, void *u) { (void)w; (void)u; spawn("/gterm"); }

int main(void) {
    wid = ag_window_create(120, 90, 560, 310, "USB Manager", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 20, AG_PANEL);

    ag_add_label(&view, 16, 14, "AuraLite USB Manager", AG_ACCENT);
    ag_add_label(&view, 16, 34, "Live view of USB hotplug + Mass Storage (/usb)", AG_DARK);
    ag_add_button(&view, 400, 12, 130, 26, "Refresh", on_refresh, 0);

    status_box   = ag_add_textbox(&view, 16, 64,  250, 24, "status: ...");
    capacity_box = ag_add_textbox(&view, 280, 64, 250, 24, "capacity: ...");
    fat_box      = ag_add_textbox(&view, 16, 96,  250, 24, "fat32: ...");
    magic_box    = ag_add_textbox(&view, 280, 96, 250, 24, "sector0: ...");

    ag_add_label(&view, 16, 132, "Info preview:", AG_BLACK);
    detail_box = ag_add_textbox(&view, 16, 150, 514, 54, "loading...");

    ag_add_button(&view, 16, 224, 150, 30, "Open File Manager", on_files, 0);
    ag_add_button(&view, 180, 224, 150, 30, "Open Terminal", on_term, 0);
    ag_add_label(&view, 16, 270, "Tip: use /usb/info, /usb/sector0.bin, /usb/disk.img, /usb/fat", AG_DARK);

    refresh_usb();
    ag_view_run(&view, 0, 0);
    return 0;
}
