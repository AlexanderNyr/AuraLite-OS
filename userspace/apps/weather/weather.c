/*
 * weather.c — live weather report for AuraLite OS.
 *
 * Fetches current conditions and forecasts from wttr.in over plain HTTP/1.0
 * (port 80) using the kernel TCP stack via the same libc primitives as the
 * bundled /apps/http client: dns_resolve + net_connect/net_send/net_recv.
 *
 * Usage (city defaults to Riga when omitted):
 *   weather [city ...]    compact summary: condition, temp, wind, humidity...
 *   weather full [city]   today's detailed ASCII-art report
 *   weather 3 [city]      3-day ASCII-art forecast
 *   weather help          this text
 *
 * wttr.in returns its iconic ASCII-art report when the User-Agent looks like
 * curl; the letter options select how much to send and how to format it:
 *   1 = today only, q = no "Weather report:" header, T = no ANSI escape
 *   codes (important: the AuraLite console is not an ANSI terminal),
 *   n = narrow layout.  In compact mode we instead pass a custom ?format=
 *   string (URL-encoded) with the fields we care about.
 *
 * Buffers live in .bss rather than on the (small) user stack; the observed
 * body sizes are ~0.4 KiB compact, ~1.5 KiB today and ~4 KiB for 3 days,
 * so 48 KiB leaves very comfortable headroom.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define HOST        "wttr.in"
#define DEFAULT_CITY "Riga"

/*
 * Compact mode format string, percent-encoded for the query line:
 *
 *   %l           location ("Riga")
 *   %C %t %f     condition text, temperature, feels-like
 *   %w %h %p     wind, humidity, precipitation
 *   %P %T        pressure (hPa), local time
 *   %S %s        sunrise, sunset
 *
 * Encoding: '%' -> %25, ' ' -> %20, newline -> %0A, '|' -> %7C, ',' -> %2C.
 */
#define FMT_COMPACT \
    "format=%25l%0A" \
    "%25C%2C%20%25t%20(feels%20%25f)%0A" \
    "Wind%20%25w%20%7C%20Humidity%20%25h%20%7C%20Precip%20%25p%0A" \
    "Pressure%20%25P%20%7C%20Local%20time%20%25T%0A" \
    "Sunrise%20%25S%20%7C%20Sunset%20%25s%0A"

static char req_buf[1280];
static char resp_buf[49152];
static char city[160];

static void usage(void) {
    puts("weather - live weather report (data: wttr.in)");
    puts("");
    puts("  weather [city ...]   compact summary (default city: Riga)");
    puts("  weather full [city]  today's detailed ASCII report");
    puts("  weather 3 [city]     3-day ASCII forecast");
    puts("  weather help         this text");
    puts("");
    puts("Examples:");
    puts("  weather");
    puts("  weather London");
    puts("  weather full New York");
    puts("  weather 3 Tokyo");
}

/* Join argv[first..] into one city token; spaces become '+', control
 * characters are dropped (an HTTP request line must stay printable). */
static void build_city(int argc, char **argv, int first) {
    int pos = 0;
    city[0] = 0;
    for (int i = first; i < argc; i++) {
        if (argv[i][0] == 0) continue;
        if (pos > 0 && pos < (int)sizeof(city) - 1) city[pos++] = '+';
        for (const char *p = argv[i]; *p && pos < (int)sizeof(city) - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == ' ') {
                if (pos > 0 && city[pos - 1] != '+') city[pos++] = '+';
            } else if (c >= 0x20) {
                city[pos++] = (char)c;
            }
        }
    }
    city[pos] = 0;
    if (city[0] == 0) strcpy(city, DEFAULT_CITY);
}

static int build_request(int mode) {
    char *w = req_buf;
    const char *p;

    for (p = "GET /"; *p; p++) *w++ = *p;
    for (p = city; *p; p++) *w++ = *p;
    *w++ = '?';
    switch (mode) {
    case 1: p = "1qTnmM";            break;   /* today, metric, m/s  */
    case 2: p = "qTnmM";             break;   /* 3-day, metric, m/s  */
    default: p = "mM&" FMT_COMPACT;  break;   /* compact summary     */
    }
    for (; *p; p++) *w++ = *p;
    for (p = " HTTP/1.0\r\nHost: " HOST "\r\n"
             "User-Agent: curl\r\n"
             "Accept: text/plain\r\n"
             "Connection: close\r\n\r\n"; *p; p++) *w++ = *p;
    *w = 0;
    return (int)(w - req_buf);
}

/* Skip the HTTP status line + headers when they are present; wttr.in's body
 * is all we want to show. */
static const char *find_body(const char *resp, int total) {
    for (int i = 0; i + 3 < total; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' &&
            resp[i + 2] == '\r' && resp[i + 3] == '\n')
            return resp + i + 4;
    }
    return resp; /* headers absent or truncated: print everything */
}

int main(int argc, char **argv) {
    int mode = 0;   /* 0 compact, 1 today, 2 three-day */
    int first = 1;

    if (argc > 1) {
        if (strcmp(argv[1], "full") == 0 || strcmp(argv[1], "today") == 0) {
            mode = 1; first = 2;
        } else if (strcmp(argv[1], "3") == 0 || strcmp(argv[1], "3d") == 0) {
            mode = 2; first = 2;
        } else if (strcmp(argv[1], "help") == 0 ||
                   strcmp(argv[1], "-h") == 0 ||
                   strcmp(argv[1], "--help") == 0) {
            usage();
            return 0;
        }
    }

    build_city(argc, argv, first);

    puts("================================================");
    printf("  AuraLite Weather - %s\n", city);
    puts("  data source: wttr.in (HTTP/1.0 via AuraLite TCP)");
    puts("================================================");

    /* 1. Resolve. */
    printf("[net] resolving %s...\n", HOST);
    uint32_t ip = dns_resolve(HOST);
    if (ip == 0) {
        printf("[error] DNS lookup failed for %s\n", HOST);
        puts("        check the network: 'nslookup wttr.in', 'ping 10.0.2.2'");
        return 1;
    }
    printf("[net] %s resolved\n", HOST);

    /* 2. Connect. */
    printf("[net] connecting to %s:80...\n", HOST);
    if (net_connect(ip, 80) != 0) {
        puts("[error] TCP connection failed (server busy? try again)");
        return 1;
    }

    /* 3. Send the request. */
    int req_len = build_request(mode);
    int sent = net_send(req_buf, (uint32_t)req_len);
    if (sent != req_len) {
        printf("[error] net_send wrote %d of %d bytes\n", sent, req_len);
        net_close();
        return 1;
    }
    printf("[net] request sent (%d bytes), waiting for weather...\n", sent);

    /* 4. Read the whole response (server closes: HTTP/1.0 semantics). */
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
    printf("[net] received %d bytes\n\n", total);

    if (total <= 0) {
        puts("[error] empty response (wttr.in may be rate-limiting; retry)");
        return 1;
    }

    /* 5. Body only, straight to the console. */
    const char *body = find_body(resp_buf, total);
    int body_len = total - (int)(body - resp_buf);
    if (body_len > 0) write(1, body, (uint32_t)body_len);
    if (body_len == 0 || body[body_len - 1] != '\n') putchar('\n');

    puts("");
    puts("------------------------------------------------");
    if (mode == 0)
        puts("tip: 'weather full <city>' = today, 'weather 3 <city>' = 3-day");
    return 0;
}
