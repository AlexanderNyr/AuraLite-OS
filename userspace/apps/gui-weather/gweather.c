/* gweather — live weather window for AuraLite OS.
 *
 * Graphical sibling of /apps/weather: fetches the compact wttr.in report for
 * Riga over HTTP/1.0 (kernel TCP stack) and shows it in a libauragui window.
 * The body lines are sanitised to printable ASCII because the GUI bitmap
 * font is byte-indexed (no UTF-8 shaping): '°' becomes blank and the UTF-8
 * wind arrows collapse to '>' / '<' / '^' / 'v'.
 *
 * Companion console app: userspace/apps/weather/weather.c (same protocol).
 */

#include "auragui.h"
#include "unistd.h"
#include "string.h"
#include "stdio.h"

#define HOST  "wttr.in"
#define CITY  "Riga"

/* Same percent-encoded compact format as the console weather app. */
#define FMT \
    "mM&format=%25l%0A" \
    "%25C%2C%20%25t%20(feels%20%25f)%0A" \
    "Wind%20%25w%20%7C%20Humidity%20%25h%20%7C%20Precip%20%25p%0A" \
    "Pressure%20%25P%20%7C%20Local%20time%20%25T%0A" \
    "Sunrise%20%25S%20%7C%20Sunset%20%25s%0A"

#define MAX_LINES 12
#define LINE_W    80

static char resp_buf[20480];
static char lines[MAX_LINES][LINE_W];
static int  nlines = 0;

/* Arrow tail byte -> simple ASCII direction. */
static char arrow_ascii(uint8_t t) {
    switch (t) {
    case 0x91: case 0x96: return '<';   /* left / north-west */
    case 0x93:           return '^';    /* up   */
    case 0x94:           return 'v';    /* down */
    default:             return '>';    /* right & diagonals */
    }
}

/* Copy one line, mapping the few UTF-8 sequences wttr.in emits to ASCII. */
static void sanitize_line(const char *s, char *d, int cap) {
    int o = 0;
    while (*s && *s != '\n' && o < cap - 1) {
        uint8_t c = (uint8_t)*s;
        if (c == '\r') { s++; continue; }
        if (c == 0xC2 && (uint8_t)s[1] == 0xB0) {   /* degree sign */
            d[o++] = ' '; s += 2; continue;
        }
        if (c == 0xE2 && (uint8_t)s[1] == 0x86) {   /* arrows */
            d[o++] = arrow_ascii((uint8_t)s[2]); s += 3; continue;
        }
        if (c >= 0x80) {                            /* any other UTF-8: drop */
            s++; continue;
        }
        d[o++] = (char)c; s++;
    }
    d[o] = 0;
}

/* Split the HTTP body into at most MAX_LINES display lines. */
static int prepare_lines(void) {
    const char *body = resp_buf;
    for (int i = 0; i + 3 < (int)sizeof(resp_buf) && body[i]; i++) {
        if (body[i] == '\r' && body[i + 1] == '\n' &&
            body[i + 2] == '\r' && body[i + 3] == '\n') {
            body += i + 4;
            break;
        }
    }
    nlines = 0;
    while (nlines < MAX_LINES && *body) {
        sanitize_line(body, lines[nlines], LINE_W);
        nlines++;
        const char *nl = strchr(body, '\n');
        if (!nl) break;
        body = nl + 1;
    }
    return nlines;
}

static int fetch_weather(void) {
    uint32_t ip = dns_resolve(HOST);
    if (ip == 0) return -1;
    if (net_connect(ip, 80) != 0) return -2;

    static char req[1024];
    char *w = req;
    const char *p;
    for (p = "GET /" CITY "?" FMT " HTTP/1.0\r\n"
             "Host: " HOST "\r\nUser-Agent: curl\r\n"
             "Accept: text/plain\r\nConnection: close\r\n\r\n"; *p; p++)
        *w++ = *p;
    *w = 0;
    int len = (int)(w - req);
    if (net_send(req, (uint32_t)len) != len) { net_close(); return -3; }

    int total = 0;
    for (;;) {
        int got = net_recv(resp_buf + total,
                           (uint32_t)(sizeof(resp_buf) - 1 - total));
        if (got <= 0) break;
        total += got;
        if (total >= (int)sizeof(resp_buf) - 1) break;
    }
    resp_buf[total] = 0;
    net_close();
    return total > 0 ? total : -4;
}

int main(void) {
    int r = fetch_weather();

    int wid = ag_window_create(340, 120, 480, 320, "Weather - Riga (wttr.in)",
                               AG_WIN_DEFAULT & ~AG_WIN_RESIZABLE);
    if (wid < 0) return 1;
    ag_window_show(wid);

    ag_widget_t buf[24];
    ag_view_t v;
    ag_view_init(&v, wid, buf, 24, 0x00F4F8FF);

    ag_add_label(&v, 24, 18, "Live weather report", AG_ACCENT);
    ag_add_label(&v, 24, 36, "source: wttr.in over HTTP/1.0 + AuraLite TCP", AG_DARK);

    if (r > 0 && prepare_lines() > 0) {
        for (int i = 0; i < nlines; i++) {
            uint32_t color = (i == 1) ? AG_BLACK : AG_DARK;
            ag_add_label(&v, 24, 70 + i * 20, lines[i], color);
        }
    } else {
        ag_add_label(&v, 24, 84,  "Could not fetch weather right now.", AG_RED);
        ag_add_label(&v, 24, 104, "Check networking, then run", AG_DARK);
        ag_add_label(&v, 24, 122, "'weather Riga' in the console shell.", AG_DARK);
    }

    ag_add_label(&v, 24, 70 + MAX_LINES * 20 - 12,
                 "console app: weather [full|3] <city>", AG_DARK);
    ag_view_run(&v, 0, 0);
    return 0;
}
