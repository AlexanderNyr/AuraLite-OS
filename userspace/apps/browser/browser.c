/*
 * browser.c — web browser for AuraLite OS.
 *
 * Fetches HTML over HTTP/1.0 via TCP, parses and renders a simplified text
 * version to the serial console. Supports:
 *   - DNS resolution + TCP connect + HTTP GET
 *   - HTML tag stripping (renders text content)
 *   - Title extraction (<title>)
 *   - Heading rendering (<h1>-<h3>)
 *   - Link listing (<a href>)
 *   - Status line display (HTTP response code)
 *
 * Usage: run /browser
 * Then type a URL like: example.com or example.com/page
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define MAX_RESPONSE 8192
#define MAX_HOSTNAME 256
#define MAX_PATH     256
#define MAX_TITLE    128

static char response[MAX_RESPONSE];
static char host_buf[MAX_HOSTNAME];
static char path_buf[MAX_PATH];

/* ---- IP to string ---- */
static void ip_str(uint32_t ip, char *buf) {
    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned o = (ip >> (i * 8)) & 0xFF;
        if (o >= 100) buf[pos++] = '0' + o / 100;
        if (o >= 10)  buf[pos++] = '0' + (o / 10) % 10;
        buf[pos++] = '0' + o % 10;
        if (i > 0) buf[pos++] = '.';
    }
    buf[pos] = 0;
}

/* ---- URL parser: host[:port]/path ---- */
static void parse_url(const char *url, char *host, char *path) {
    /* Skip http:// prefix if present. */
    const char *s = url;
    if (strncmp(s, "http://", 7) == 0) s += 7;

    int hpos = 0;
    while (*s && *s != '/' && *s != ':' && hpos < MAX_HOSTNAME - 1) {
        host[hpos++] = *s++;
    }
    host[hpos] = 0;

    /* Skip port if present. */
    if (*s == ':') {
        s++;
        while (*s && *s != '/') s++;
    }

    /* Path. */
    if (*s == '/') {
        int ppos = 0;
        while (*s && ppos < MAX_PATH - 1) {
            path[ppos++] = *s++;
        }
        path[ppos] = 0;
    } else {
        path[0] = '/';
        path[1] = 0;
    }
}

/* ---- HTTP GET ---- */
static int http_get(const char *host, const char *path, char *buf, int bufsize) {
    /* Resolve. */
    uint32_t ip = dns_resolve(host);
    if (ip == 0) {
        printf("  Cannot resolve '%s'\n", host);
        return -1;
    }
    char ipstr[20];
    ip_str(ip, ipstr);
    printf("  Resolved: %s (%s)\n", host, ipstr);

    /* Connect. */
    if (net_connect(ip, 80) != 0) {
        printf("  Connection failed\n");
        return -1;
    }

    /* Build HTTP request. */
    char req[512];
    int rlen = 0;
    const char *tmpl = "GET ";
    while (*tmpl) req[rlen++] = *tmpl++;
    int plen = 0;
    while (path[plen] && rlen < 400) req[rlen++] = path[plen++];
    const char *hdr = " HTTP/1.0\r\nHost: ";
    while (*hdr) req[rlen++] = *hdr++;
    int hlen = 0;
    while (host[hlen] && rlen < 480) req[rlen++] = host[hlen++];
    req[rlen++] = '\r';
    req[rlen++] = '\n';
    req[rlen++] = '\r';
    req[rlen++] = '\n';

    /* Send. */
    int sent = net_send(req, rlen);
    if (sent <= 0) {
        printf("  Send failed\n");
        net_close();
        return -1;
    }

    /* Receive. */
    int total = 0;
    for (;;) {
        int got = net_recv(buf + total, bufsize - total - 1);
        if (got <= 0) break;
        total += got;
        if (total >= bufsize - 1) break;
    }
    buf[total] = 0;
    net_close();

    if (total == 0) {
        printf("  No response received\n");
        return -1;
    }

    printf("  Received %d bytes\n", total);
    return total;
}

/* ---- Simple HTML renderer ---- */

static int in_tag = 0;
static int in_script = 0;
static int in_style = 0;

/* Check if a tag name matches (case-insensitive, length-bounded). */
static int tag_is(const char *tag, const char *name) {
    for (int i = 0; name[i]; i++) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') c += 32;  /* tolower */
        if (c != name[i]) return 0;
    }
    /* Check the next char is a boundary (space, >, /). */
    char next = tag[strlen(name)];
    return (next == ' ' || next == '>' || next == '/' || next == '\t' ||
            next == '\n' || next == 0);
}

/* Extract attribute value from a tag (e.g. href="..." in <a>). */
static void extract_attr(const char *tag, const char *attr, char *out, int maxlen) {
    out[0] = 0;
    /* Search for attr=" in the tag. */
    char pattern[32];
    /* Build: attr=" */
    int plen = 0;
    for (int i = 0; attr[i]; i++) {
        pattern[plen++] = attr[i];
    }
    pattern[plen++] = '=';
    pattern[plen++] = '"';
    pattern[plen] = 0;

    /* Find pattern in tag (case-insensitive). */
    int taglen = strlen(tag);
    for (int i = 0; i + plen <= taglen; i++) {
        int match = 1;
        for (int j = 0; j < plen; j++) {
            char c = tag[i + j];
            char d = pattern[j];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != d) { match = 0; break; }
        }
        if (match) {
            /* Copy the value until closing quote. */
            int vstart = i + plen;
            int vlen = 0;
            while (tag[vstart + vlen] && tag[vstart + vlen] != '"' &&
                   vlen < maxlen - 1) {
                out[vlen] = tag[vstart + vlen];
                vlen++;
            }
            out[vlen] = 0;
            return;
        }
    }
}

static void render_html(const char *html) {
    in_tag = 0;
    in_script = 0;
    in_style = 0;

    int blank = 0;  /* suppress consecutive blank lines */

    const char *p = html;
    char tag_buf[256];
    int tag_pos;

    /* Skip HTTP headers (up to \r\n\r\n). */
    const char *body = html;
    while (*body) {
        if (body[0] == '\r' && body[1] == '\n' &&
            body[2] == '\r' && body[3] == '\n') {
            body += 4;
            break;
        }
        if (body[0] == '\n' && body[1] == '\n') {
            body += 2;
            break;
        }
        body++;
    }

    /* Print status line. */
    if (html[0] == 'H' && html[1] == 'T' && html[2] == 'T' && html[3] == 'P') {
        /* Find end of first line. */
        int eol = 0;
        while (html[eol] && html[eol] != '\r' && html[eol] != '\n') eol++;
        printf("  Status: ");
        for (int i = 0; i < eol && i < 80; i++) putchar(html[i]);
        putchar('\n');
    }

    p = body;

    while (*p) {
        if (*p == '<') {
            /* Start of a tag. */
            tag_pos = 0;
            p++;
            if (*p == '/') p++;
            while (*p && *p != '>' && tag_pos < 255) {
                tag_buf[tag_pos++] = *p++;
            }
            tag_buf[tag_pos] = 0;
            if (*p == '>') p++;

            /* Process tag. */
            if (tag_is(tag_buf, "title")) {
                /* Print title specially. */
                char title[MAX_TITLE];
                int tlen = 0;
                while (*p && *p != '<' && tlen < MAX_TITLE - 1) {
                    title[tlen++] = *p++;
                }
                title[tlen] = 0;
                printf("\n  === %s ===\n\n", title);
                blank = 0;
            } else if (tag_is(tag_buf, "h1") || tag_is(tag_buf, "h2") ||
                       tag_is(tag_buf, "h3")) {
                putchar('\n');
                blank = 0;
            } else if (tag_is(tag_buf, "p") || tag_is(tag_buf, "br") ||
                       tag_is(tag_buf, "div") || tag_is(tag_buf, "li")) {
                if (!blank) {
                    putchar('\n');
                    blank = 1;
                }
            } else if (tag_is(tag_buf, "a ")) {
                /* Extract href. */
                char href[256];
                extract_attr(tag_buf, "href", href, sizeof(href));
                if (href[0]) {
                    printf(" [");
                    int hlen = strlen(href);
                    if (hlen > 40) hlen = 40;
                    for (int i = 0; i < hlen; i++) putchar(href[i]);
                    printf("] ");
                }
            } else if (tag_is(tag_buf, "script")) {
                in_script = 1;
            } else if (tag_is(tag_buf, "/script")) {
                in_script = 0;
            } else if (tag_is(tag_buf, "style")) {
                in_style = 1;
            } else if (tag_is(tag_buf, "/style")) {
                in_style = 0;
            }
        } else if (*p == '>') {
            p++;
        } else if (*p == '&') {
            /* HTML entities. */
            if (strncmp(p, "&amp;", 5) == 0) { putchar('&'); p += 5; }
            else if (strncmp(p, "&lt;", 4) == 0) { putchar('<'); p += 4; }
            else if (strncmp(p, "&gt;", 4) == 0) { putchar('>'); p += 4; }
            else if (strncmp(p, "&nbsp;", 6) == 0) { putchar(' '); p += 6; }
            else if (strncmp(p, "&quot;", 6) == 0) { putchar('"'); p += 6; }
            else if (strncmp(p, "&#39;", 5) == 0) { putchar('\''); p += 5; }
            else { p++; }
            blank = 0;
        } else if (!in_script && !in_style) {
            /* Regular text. */
            char c = *p++;
            /* Collapse whitespace. */
            if (c == '\t' || c == '\r') c = ' ';
            if (c == ' ' && blank) {
                /* skip */
            } else {
                if (c == '\n') {
                    if (!blank) {
                        putchar('\n');
                        blank = 1;
                    }
                } else {
                    putchar(c);
                    blank = 0;
                }
            }
        } else {
            /* Inside script/style — skip. */
            p++;
        }
    }
    putchar('\n');
}

int main(void) {
    puts("=== AuraLite Web Browser ===");
    puts("Enter a URL (e.g. example.com or example.com/page)");
    puts("Type 'quit' to exit.");
    puts("");

    for (;;) {
        write(1, "browser> ", 9);
        int64_t n = read(0, host_buf, sizeof(host_buf) - 1);
        if (n <= 0) continue;
        host_buf[n] = 0;
        if (n > 0 && host_buf[n-1] == '\n') host_buf[n-1] = 0;
        if (host_buf[0] == 0) continue;

        if (strcmp(host_buf, "quit") == 0 || strcmp(host_buf, "q") == 0) break;

        /* Parse the URL into host + path. */
        parse_url(host_buf, host_buf, path_buf);

        printf("Fetching http://%s%s ...\n", host_buf, path_buf);

        /* Fetch the page. */
        int len = http_get(host_buf, path_buf, response, MAX_RESPONSE);
        if (len <= 0) {
            puts("Failed to fetch page.\n");
            continue;
        }

        /* Render. */
        puts("----------------------------------------");
        render_html(response);
        puts("----------------------------------------\n");
    }

    puts("Goodbye from AuraLite Browser!");
    return 0;
}
