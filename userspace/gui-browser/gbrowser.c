/*
 * gbrowser.c — graphical web browser for AuraLite OS.
 *
 * Provides a proper web browser with:
 *   - URL bar (textbox), "Go" button, "Back" button, and "Home" button
 *   - HTML-to-text renderer with link extraction
 *   - Clickable links for page-to-page navigation
 *   - Scrollable Listbox content view
 */

#include "auragui.h"
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

#define MAX_RESPONSE 16384
#define MAX_LINE_LEN 52
#define MAX_LINES 128

static int wid;
static ag_widget_t widgets[30];
static ag_view_t view;

static ag_widget_t *url_box, *status_lbl, *list_box;

static char response[MAX_RESPONSE];

static char line_links[MAX_LINES][256];
static int line_count = 0;

/* History stack for the "Back" button */
static char history[10][256];
static int history_ptr = 0;

/* Forward declarations */
static void navigate(const char *url, int save_to_history);
static void render_html_to_listbox(const char *html, const char *current_url);

/* Parse URL: http://host[:port]/path */
static void parse_url(const char *url, char *host, char *path, uint16_t *port) {
    const char *s = url;
    if (strncmp(s, "http://", 7) == 0) s += 7;

    int hpos = 0;
    while (*s && *s != '/' && *s != ':' && hpos < 255) {
        host[hpos++] = *s++;
    }
    host[hpos] = 0;

    *port = 80;
    if (*s == ':') {
        s++;
        uint32_t p = 0;
        while (*s && *s >= '0' && *s <= '9') {
            p = p * 10 + (*s - '0');
            s++;
        }
        if (p > 0 && p <= 65535) *port = (uint16_t)p;
    }

    if (*s == '/') {
        int ppos = 0;
        while (*s && ppos < 255) {
            path[ppos++] = *s++;
        }
        path[ppos] = 0;
    } else {
        path[0] = '/';
        path[1] = 0;
    }
}

/* Resolve links (absolute / relative) */
static void resolve_link(const char *current_url, const char *link, char *out_url) {
    if (strncmp(link, "http://", 7) == 0) {
        strcpy(out_url, link);
        return;
    }
    
    char host[256];
    char path[256];
    uint16_t port;
    parse_url(current_url, host, path, &port);

    if (link[0] == '/') {
        /* Absolute path on same host */
        strcpy(out_url, "http://");
        strcat(out_url, host);
        if (port != 80) {
            strcat(out_url, ":");
            char pstr[10];
            int p = port, i = 0;
            while (p > 0) { pstr[i++] = '0' + (p % 10); p /= 10; }
            pstr[i] = 0;
            for (int j = 0; j < i / 2; j++) {
                char tmp = pstr[j]; pstr[j] = pstr[i - 1 - j]; pstr[i - 1 - j] = tmp;
            }
            strcat(out_url, pstr);
        }
        strcat(out_url, link);
    } else {
        /* Relative path on same host */
        int last_slash = -1;
        for (int i = 0; path[i]; i++) {
            if (path[i] == '/') last_slash = i;
        }
        char parent[256];
        if (last_slash >= 0) {
            strncpy(parent, path, last_slash + 1);
            parent[last_slash + 1] = 0;
        } else {
            strcpy(parent, "/");
        }
        
        strcpy(out_url, "http://");
        strcat(out_url, host);
        if (port != 80) {
            strcat(out_url, ":");
            char pstr[10];
            int p = port, i = 0;
            while (p > 0) { pstr[i++] = '0' + (p % 10); p /= 10; }
            pstr[i] = 0;
            for (int j = 0; j < i / 2; j++) {
                char tmp = pstr[j]; pstr[j] = pstr[i - 1 - j]; pstr[i - 1 - j] = tmp;
            }
            strcat(out_url, pstr);
        }
        strcat(out_url, parent);
        strcat(out_url, link);
    }
}

/* HTTP GET fetcher */
static int http_get(const char *host, const char *path, uint16_t port, char *buf, int bufsize) {
    uint32_t ip = dns_resolve(host);
    if (ip == 0) return -1;
    
    if (net_connect(ip, port) != 0) return -1;
    
    /* Build HTTP request */
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
    
    int sent = net_send(req, rlen);
    if (sent <= 0) {
        net_close();
        return -1;
    }
    
    int total = 0;
    for (;;) {
        int got = net_recv(buf + total, bufsize - total - 1);
        if (got <= 0) break;
        total += got;
        if (total >= bufsize - 1) break;
    }
    buf[total] = 0;
    net_close();
    
    return total;
}

/* Main Navigation logic */
static void navigate(const char *url, int save_to_history) {
    /* Push current URL to history if requested */
    if (save_to_history && history_ptr < 9) {
        /* Avoid duplicate history entries */
        if (history_ptr == 0 || strcmp(history[history_ptr - 1], url_box->text) != 0) {
            strcpy(history[history_ptr++], url_box->text);
        }
    }

    ag_textbox_set(url_box, url);
    ag_textbox_set(status_lbl, "Resolving DNS & Connecting...");
    ag_view_render(&view);
    
    char host[256];
    char path[256];
    uint16_t port;
    parse_url(url, host, path, &port);
    
    int len = http_get(host, path, port, response, MAX_RESPONSE);
    if (len <= 0) {
        ag_textbox_set(status_lbl, "Connection failed or page not found.");
        ag_listbox_clear(list_box);
        ag_listbox_add(list_box, "The browser could not load the webpage.");
        ag_listbox_add(list_box, "Please check:");
        ag_listbox_add(list_box, "  - Host is reachable (HTTP port 80 only)");
        ag_listbox_add(list_box, "  - DNS is resolved properly");
        ag_view_render(&view);
        return;
    }
    
    ag_textbox_set(status_lbl, "Rendering page...");
    ag_view_render(&view);
    
    ag_listbox_clear(list_box);
    memset(line_links, 0, sizeof(line_links));
    line_count = 0;
    
    render_html_to_listbox(response, url);
    
    ag_textbox_set(status_lbl, "Ready");
    ag_view_render(&view);
}

/* UI Actions */
static void on_go(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    navigate(url_box->text, 1);
}

static void on_back(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    if (history_ptr > 0) {
        char prev[256];
        strcpy(prev, history[--history_ptr]);
        navigate(prev, 0);
    } else {
        ag_textbox_set(status_lbl, "No history.");
        ag_view_render(&view);
    }
}

static void on_home(ag_widget_t *w, void *u) {
    (void)w; (void)u;
    navigate("neverssl.com", 1);
}

static void on_select_line(ag_widget_t *w, void *u) {
    (void)u;
    int idx = w->selected;
    if (idx >= 0 && idx < line_count && line_links[idx][0] != 0) {
        /* Navigate to clicked link! */
        char target[256];
        strcpy(target, line_links[idx]);
        navigate(target, 1);
    }
}

/* Add a parsed HTML line to the browser listbox with wrapping support */
static void add_browser_line(const char *text, const char *link) {
    if (line_count >= MAX_LINES) return;
    
    int len = strlen(text);
    if (len <= MAX_LINE_LEN) {
        ag_listbox_add(list_box, text);
        if (link && link[0]) {
            strncpy(line_links[line_count], link, 255);
        } else {
            line_links[line_count][0] = 0;
        }
        line_count++;
    } else {
        int start = 0;
        while (start < len && line_count < MAX_LINES) {
            char chunk[MAX_LINE_LEN + 1];
            int size = len - start;
            if (size > MAX_LINE_LEN) size = MAX_LINE_LEN;
            strncpy(chunk, text + start, size);
            chunk[size] = 0;
            
            ag_listbox_add(list_box, chunk);
            if (link && link[0]) {
                strncpy(line_links[line_count], link, 255);
            } else {
                line_links[line_count][0] = 0;
            }
            line_count++;
            start += size;
        }
    }
}

static int tag_is_case(const char *tag, const char *name) {
    for (int i = 0; name[i]; i++) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != name[i]) return 0;
    }
    char next = tag[strlen(name)];
    return (next == ' ' || next == '>' || next == '/' || next == '\t' ||
            next == '\n' || next == 0);
}

static void extract_attr_case(const char *tag, const char *attr, char *out, int maxlen) {
    out[0] = 0;
    char pattern[32];
    int plen = 0;
    for (int i = 0; attr[i]; i++) {
        pattern[plen++] = attr[i];
    }
    pattern[plen++] = '=';
    pattern[plen++] = '"';
    pattern[plen] = 0;

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

/* Parse HTML and render formatted items to Listbox */
static void render_html_to_listbox(const char *html, const char *current_url) {
    int in_script = 0;
    int in_style = 0;
    
    /* Skip HTTP headers */
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
    
    /* Display HTTP status code at the very top of page. */
    if (html[0] == 'H' && html[1] == 'T' && html[2] == 'T' && html[3] == 'P') {
        int eol = 0;
        while (html[eol] && html[eol] != '\r' && html[eol] != '\n') eol++;
        char status_line[128];
        int sl_len = eol < 120 ? eol : 120;
        strncpy(status_line, html, sl_len);
        status_line[sl_len] = 0;
        add_browser_line(status_line, 0);
        add_browser_line("", 0);
    }
    
    const char *p = body;
    char current_line[1024];
    int line_pos = 0;
    
    char current_link[256];
    current_link[0] = 0;
    
    char tag_buf[256];
    int tag_pos;
    
    while (*p && line_count < MAX_LINES) {
        if (*p == '<') {
            /* Start of a tag */
            tag_pos = 0;
            p++;
            int is_closing = 0;
            if (*p == '/') {
                is_closing = 1;
                p++;
            }
            while (*p && *p != '>' && tag_pos < 255) {
                tag_buf[tag_pos++] = *p++;
            }
            tag_buf[tag_pos] = 0;
            if (*p == '>') p++;
            
            /* Process tag */
            if (tag_is_case(tag_buf, "title") && !is_closing) {
                char title[128];
                int tlen = 0;
                while (*p && *p != '<' && tlen < 127) {
                    title[tlen++] = *p++;
                }
                title[tlen] = 0;
                char win_title[256];
                strcpy(win_title, "Browser - ");
                strcat(win_title, title);
                ag_window_set_title(wid, win_title);
            } else if (tag_is_case(tag_buf, "h1") || tag_is_case(tag_buf, "h2") || tag_is_case(tag_buf, "h3")) {
                if (line_pos > 0) {
                    current_line[line_pos] = 0;
                    add_browser_line(current_line, current_link);
                    line_pos = 0;
                }
                if (is_closing) {
                    add_browser_line("", 0);
                } else {
                    add_browser_line("----------------------------------------", 0);
                }
            } else if (tag_is_case(tag_buf, "p") || tag_is_case(tag_buf, "br") ||
                       tag_is_case(tag_buf, "div") || tag_is_case(tag_buf, "li")) {
                if (line_pos > 0) {
                    current_line[line_pos] = 0;
                    add_browser_line(current_line, current_link);
                    line_pos = 0;
                }
                if (tag_is_case(tag_buf, "br")) {
                    /* skip double newline */
                } else if (!is_closing) {
                    /* skip */
                }
            } else if (tag_is_case(tag_buf, "a") && !is_closing) {
                char href[256];
                extract_attr_case(tag_buf, "href", href, sizeof(href));
                if (href[0]) {
                    resolve_link(current_url, href, current_link);
                    const char *link_indicator = "-> [LINK] ";
                    for (int i = 0; link_indicator[i] && line_pos < 1000; i++) {
                        current_line[line_pos++] = link_indicator[i];
                    }
                }
            } else if (tag_is_case(tag_buf, "a") && is_closing) {
                current_link[0] = 0;
            } else if (tag_is_case(tag_buf, "script")) {
                in_script = 1;
            } else if (tag_is_case(tag_buf, "/script")) {
                in_script = 0;
            } else if (tag_is_case(tag_buf, "style")) {
                in_style = 1;
            } else if (tag_is_case(tag_buf, "/style")) {
                in_style = 0;
            }
        } else if (*p == '>') {
            p++;
        } else if (*p == '&') {
            char entity_char = 0;
            if (strncmp(p, "&amp;", 5) == 0) { entity_char = '&'; p += 5; }
            else if (strncmp(p, "&lt;", 4) == 0) { entity_char = '<'; p += 4; }
            else if (strncmp(p, "&gt;", 4) == 0) { entity_char = '>'; p += 4; }
            else if (strncmp(p, "&nbsp;", 6) == 0) { entity_char = ' '; p += 6; }
            else if (strncmp(p, "&quot;", 6) == 0) { entity_char = '"'; p += 6; }
            else if (strncmp(p, "&#39;", 5) == 0) { entity_char = '\''; p += 5; }
            else { entity_char = *p++; }
            
            if (entity_char && !in_script && !in_style && line_pos < 1000) {
                current_line[line_pos++] = entity_char;
            }
        } else if (!in_script && !in_style) {
            char c = *p++;
            if (c == '\t' || c == '\r') c = ' ';
            if (c == '\n') {
                if (line_pos > 0) {
                    current_line[line_pos] = 0;
                    add_browser_line(current_line, current_link);
                    line_pos = 0;
                }
            } else {
                if (c == ' ' && line_pos > 0 && current_line[line_pos - 1] == ' ') {
                    /* skip multiple spaces */
                } else if (line_pos < 1000) {
                    current_line[line_pos++] = c;
                }
            }
        } else {
            p++;
        }
    }
    
    if (line_pos > 0 && line_count < MAX_LINES) {
        current_line[line_pos] = 0;
        add_browser_line(current_line, current_link);
    }
}

int main(void) {
    wid = ag_window_create(20, 20, 480, 400, "Web Browser", AG_WIN_DEFAULT);
    if (wid < 0) return 1;
    ag_window_show(wid);
    
    ag_view_init(&view, wid, widgets, 30, AG_PANEL);

    ag_add_label(&view, 12, 14, "AuraLite Web Browser", AG_ACCENT);
    
    ag_add_label(&view, 12, 38, "URL:", AG_DARK);
    url_box = ag_add_textbox(&view, 48, 34, 230, 22, "neverssl.com");
    
    ag_add_button(&view, 286, 34, 36, 22, "Go", on_go, 0);
    ag_add_button(&view, 326, 34, 42, 22, "Back", on_back, 0);
    ag_add_button(&view, 372, 34, 42, 22, "Home", on_home, 0);
    
    status_lbl = ag_add_textbox(&view, 12, 62, 456, 16, "Ready");
    
    list_box = ag_add_listbox(&view, 12, 82, 456, 290);
    list_box->on_select = on_select_line;

    /* Navigate to default homepage on load */
    navigate("neverssl.com", 0);

    ag_view_run(&view, 0, 0);
    return 0;
}
