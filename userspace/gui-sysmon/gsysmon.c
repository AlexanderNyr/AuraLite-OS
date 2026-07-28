/* gsysmon — graphical system monitor for AuraLite OS.
 *
 * Shows real, live data pulled from /proc instead of placeholder numbers:
 *   - CPU load  : /proc/loadavg (1-min busy% field, backed by the kernel's
 *                 real idle-vs-total PIT tick counters in kernel/proc/scheduler.c)
 *   - Memory    : /proc/meminfo (PMM frame accounting)
 *   - Network   : /proc/netdev  (cumulative bytes on the active NIC, sampled
 *                 twice a refresh apart to derive a rough "activity" gauge)
 *   - Disk      : /proc/diskstats (cumulative AHCI sectors read+written,
 *                 same delta-over-time approach as the network gauge)
 *   - Processes : real listing from /proc/<pid>/{cmdline,stat}, not a
 *                 hardcoded 3-line stub.
 */
#include "auragui.h"
#include "unistd.h"
#include "fcntl.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "time.h"

static int wid;
static ag_widget_t widgets[32];
static ag_view_t view;
static ag_widget_t *cpu_bar, *mem_bar, *net_bar, *disk_bar;
static ag_widget_t *cpu_lbl, *mem_lbl, *net_lbl, *disk_lbl;
static ag_widget_t *proc_lbl;
static ag_widget_t *proc_lines[10];
static char proc_line_text[10][48];

/* Read an entire small /proc file into buf (NUL-terminated). Returns the
 * number of bytes read (excluding the NUL), or -1 on error. AuraLite's
 * /proc files are all well under 1 KiB, so a single read() is enough --
 * no need for a read loop. */
static int read_proc_file(const char *path, char *buf, int bufsize) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int64_t n = read(fd, buf, (size_t)(bufsize - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return (int)n;
}

/* Parse the first field of /proc/loadavg ("busy%.frac ...") into an integer
 * percentage 0-100, rounded to the nearest whole percent. */
static int read_cpu_percent(void) {
    char buf[64];
    if (read_proc_file("/proc/loadavg", buf, sizeof(buf)) <= 0) return 0;
    int whole = 0, frac = 0;
    sscanf(buf, "%d.%d", &whole, &frac);
    int pct = whole + (frac >= 50 ? 1 : 0);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Parse "MemTotal: N KiB" / "MemUsed: N KiB" out of /proc/meminfo and return
 * used-memory percentage 0-100. */
static int read_mem_percent(void) {
    char buf[512];
    if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) <= 0) return 0;
    unsigned long long total = 0, used = 0;
    char *p = strstr(buf, "MemTotal:");
    if (p) sscanf(p, "MemTotal: %llu", &total);
    p = strstr(buf, "MemUsed:");
    if (p) sscanf(p, "MemUsed: %llu", &used);
    if (total == 0) return 0;
    int pct = (int)((used * 100ULL) / total);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Pull "rx_bytes tx_bytes" (cumulative, since boot) out of /proc/netdev's
 * single data row. */
static void read_net_bytes(unsigned long long *rx, unsigned long long *tx) {
    *rx = 0; *tx = 0;
    char buf[256];
    if (read_proc_file("/proc/netdev", buf, sizeof(buf)) <= 0) return;
    /* Row format: "<iface>: <rxbytes> <rxpackets> <txbytes> <txpackets>" --
     * find the line containing ':' after the two header lines. */
    char *colon = strchr(buf, ':');
    /* First colon is inside the header ("Inter-|"); skip to the real data
     * row's colon, which is the one whose preceding token is the iface
     * name (no '|' nearby). Simplify: take the LAST colon in the buffer. */
    char *last_colon = 0;
    for (char *s = buf; *s; s++) if (*s == ':') last_colon = s;
    colon = last_colon;
    if (!colon) return;
    sscanf(colon + 1, "%llu %*u %llu", rx, tx);
}

/* Pull cumulative sectors-read/written out of /proc/diskstats's single
 * data row (see ahci_get_stats()). Linux diskstats field layout is
 * "major minor name rd_ios rd_merges rd_sectors ... wr_ios wr_merges
 * wr_sectors ..."; AuraLite's simplified row keeps the same column
 * positions for rd_sectors (6th field) and wr_sectors (10th field) so any
 * real diskstats parser would also work here. */
static void read_disk_sectors(unsigned long long *rd, unsigned long long *wr) {
    *rd = 0; *wr = 0;
    char buf[256];
    if (read_proc_file("/proc/diskstats", buf, sizeof(buf)) <= 0) return;
    unsigned long maj, min;
    char name[32];
    unsigned long long f4, f5, f6, f7, f8, f9;
    int n = sscanf(buf, "%lu %lu %31s %llu %llu %llu %llu %llu %llu",
                   &maj, &min, name, &f4, &f5, &f6, &f7, &f8, &f9);
    if (n >= 9) { *rd = f6; *wr = f9; }
}

/* Convert a byte delta over one refresh interval into a coarse 0-100
 * "activity" percentage for the progress bar. There is no fixed hardware
 * bandwidth ceiling to normalise against (QEMU's virtual NIC/disk speeds
 * vary by backend), so this uses a a simple saturating log-ish scale: any
 * measurable traffic shows as at least a sliver, heavier bursts fill the
 * bar further, purely to give a relative sense of "some/more/a lot" of
 * activity rather than a fabricated absolute percentage. */
static int activity_percent(unsigned long long delta, unsigned long long scale) {
    if (delta == 0) return 0;
    unsigned long long pct = (delta * 100ULL) / (scale ? scale : 1);
    if (pct > 100) pct = 100;
    if (pct < 2) pct = 2; /* keep a visible sliver so "some activity" is legible */
    return (int)pct;
}

/* itoa-free decimal formatting helper (no size_t/format-string dependency
 * issues across the freestanding libc's varying printf support). */
static void append_uint(char *dst, int *pos, unsigned long v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) dst[(*pos)++] = tmp[--n];
}

static void refresh_process_list(void) {
    int count = 0;
    for (int pid = 1; pid < 64 && count < 10; pid++) {
        char path[64];
        int p = 0;
        strcpy(path, "/proc/");
        p = 6;
        append_uint(path, &p, (unsigned long)pid);
        path[p] = '\0';
        strcat(path, "/cmdline");

        char name[40];
        int n = read_proc_file(path, name, sizeof(name));
        if (n <= 0) continue;
        if (n > 0 && name[n - 1] == '\n') name[n - 1] = '\0';
        if (name[0] == '\0') continue;

        /* Pull the running/ready/blocked state letter out of /proc/<pid>/stat
         * (3rd whitespace-separated field, Linux-style: R/r/S/Z). */
        char statpath[64];
        p = 6;
        strcpy(statpath, "/proc/");
        append_uint(statpath, &p, (unsigned long)pid);
        statpath[p] = '\0';
        strcat(statpath, "/stat");
        char statbuf[128];
        char state = '?';
        if (read_proc_file(statpath, statbuf, sizeof(statbuf)) > 0) {
            char comm[40];
            unsigned long long ppid = 0, quantum = 0;
            sscanf(statbuf, "%*llu %39s %c %llu %llu", comm, &state, &ppid, &quantum);
        }

        char *buf = proc_line_text[count];
        int op = 0;
        strcpy(buf, "PID ");
        op = 4;
        append_uint(buf, &op, (unsigned long)pid);
        buf[op++] = ' ';
        buf[op++] = '[';
        buf[op++] = state;
        buf[op++] = ']';
        buf[op++] = ' ';
        buf[op] = '\0';
        strcat(buf, name);
        proc_lines[count]->text[0] = '\0';
        strncpy(proc_lines[count]->text, buf, AG_MAX_WIDGET_TEXT - 1);
        proc_lines[count]->text[AG_MAX_WIDGET_TEXT - 1] = '\0';
        count++;
    }
    for (int i = count; i < 10; i++) {
        proc_lines[i]->text[0] = '\0';
    }
    char hdr[40];
    strcpy(hdr, "Processes (");
    int hp = (int)strlen(hdr);
    append_uint(hdr, &hp, (unsigned long)count);
    hdr[hp] = '\0';
    strcat(hdr, "):");
    strncpy(proc_lbl->text, hdr, AG_MAX_WIDGET_TEXT - 1);
    proc_lbl->text[AG_MAX_WIDGET_TEXT - 1] = '\0';
}

static unsigned long long last_rx, last_tx, last_rd, last_wr;

static void refresh_all(void) {
    int cpu_pct = read_cpu_percent();
    int mem_pct = read_mem_percent();

    unsigned long long rx, tx, rd, wr;
    read_net_bytes(&rx, &tx);
    read_disk_sectors(&rd, &wr);

    unsigned long long net_delta  = (rx + tx) - (last_rx + last_tx);
    unsigned long long disk_delta = (rd + wr) - (last_rd + last_wr);
    last_rx = rx; last_tx = tx; last_rd = rd; last_wr = wr;

    /* Scale constants are rough: a few KiB/refresh of network traffic or a
     * handful of sectors of disk I/O already counts as "busy" for a hobby
     * OS with no sustained background load -- see activity_percent()'s
     * comment for why this is relative, not an absolute bandwidth reading. */
    int net_pct  = activity_percent(net_delta, 8192);
    int disk_pct = activity_percent(disk_delta, 64);

    cpu_bar->value  = cpu_pct;
    mem_bar->value  = mem_pct;
    net_bar->value  = net_pct;
    disk_bar->value = disk_pct;

    char buf[24];
    int p;
    p = 0; strcpy(buf, "CPU:   "); p = 7; append_uint(buf, &p, (unsigned long)cpu_pct); buf[p++]='%'; buf[p]=0;
    strncpy(cpu_lbl->text, buf, AG_MAX_WIDGET_TEXT - 1);
    p = 0; strcpy(buf, "Mem:   "); p = 7; append_uint(buf, &p, (unsigned long)mem_pct); buf[p++]='%'; buf[p]=0;
    strncpy(mem_lbl->text, buf, AG_MAX_WIDGET_TEXT - 1);
    p = 0; strcpy(buf, "Net:   "); p = 7; append_uint(buf, &p, (unsigned long)net_pct); buf[p++]='%'; buf[p]=0;
    strncpy(net_lbl->text, buf, AG_MAX_WIDGET_TEXT - 1);
    p = 0; strcpy(buf, "Disk:  "); p = 7; append_uint(buf, &p, (unsigned long)disk_pct); buf[p++]='%'; buf[p]=0;
    strncpy(disk_lbl->text, buf, AG_MAX_WIDGET_TEXT - 1);

    refresh_process_list();
}

int main(void) {
    wid = ag_window_create(380, 70, 360, 420, "System Monitor", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    ag_view_init(&view, wid, widgets, 32, AG_PANEL);

    ag_add_label(&view, 12,  14, "AuraLite System Monitor", AG_DARK);
    cpu_lbl  = ag_add_label(&view, 12,  40, "CPU:   ", AG_BLACK);
    cpu_bar  = ag_add_progress(&view, 90, 36, 250, 100, 0);
    mem_lbl  = ag_add_label(&view, 12,  74, "Mem:   ", AG_BLACK);
    mem_bar  = ag_add_progress(&view, 90, 70, 250, 100, 0);
    net_lbl  = ag_add_label(&view, 12, 108, "Net:   ", AG_BLACK);
    net_bar  = ag_add_progress(&view, 90, 104, 250, 100, 0);
    disk_lbl = ag_add_label(&view, 12, 142, "Disk:  ", AG_BLACK);
    disk_bar = ag_add_progress(&view, 90, 138, 250, 100, 0);

    proc_lbl = ag_add_label(&view, 12, 180, "Processes:", AG_DARK);
    for (int i = 0; i < 10; i++) {
        proc_line_text[i][0] = '\0';
        proc_lines[i] = ag_add_label(&view, 12, 200 + i * 18, proc_line_text[i], AG_BLACK);
    }

    refresh_all();
    ag_view_render(&view);

    /* Refresh roughly twice a second: fast enough to feel "live" without
     * spamming /proc reads or the compositor every frame. */
    for (;;) {
        ag_event_t e;
        int closed = 0;
        while (ag_poll_event(wid, &e)) {
            if (ag_view_dispatch(&view, &e)) { closed = 1; }
        }
        if (closed) { ag_window_destroy(wid); return 0; }
        refresh_all();
        ag_view_render(&view);
        usleep(500000);
    }
    return 0;
}
