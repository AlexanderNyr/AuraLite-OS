/*
 * http.c — real HTTP/1.0 client for AuraLite OS.
 *
 * Performs a TCP connection to a web server and sends an HTTP GET request.
 * Uses the kernel's TCP stack via syscalls (net_connect/net_send/net_recv).
 *
 * Usage: run /http
 * Then type a hostname (e.g. example.com) to fetch its homepage.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static char hostname[128];
static char request[256];
static char response[4096];

/* Convert a uint32 IP (host byte order) to dotted string. */
static void ip_to_str(uint32_t ip, char *buf) {
    /* Use unsigned to avoid UB with shifts. */
    unsigned a = (ip >> 24) & 0xFF;
    unsigned b = (ip >> 16) & 0xFF;
    unsigned c = (ip >> 8) & 0xFF;
    unsigned d = ip & 0xFF;
    /* Manual formatting (no snprintf). */
    char tmp[20];
    int pos = 0;
    /* a */
    if (a >= 100) { tmp[pos++] = '0' + a / 100; a %= 100; }
    if (a >= 10 || tmp[0]) { tmp[pos++] = '0' + a / 10; a %= 10; }
    tmp[pos++] = '0' + a;
    tmp[pos++] = '.';
    b = (ip >> 16) & 0xFF;
    if (b >= 100) { tmp[pos++] = '0' + b / 100; b %= 100; }
    if (b >= 10 || tmp[pos-1] == '0') { tmp[pos++] = '0' + b / 10; b %= 10; }
    tmp[pos++] = '0' + b;
    tmp[pos++] = '.';
    c = (ip >> 8) & 0xFF;
    if (c >= 100) { tmp[pos++] = '0' + c / 100; c %= 100; }
    if (c >= 10 || tmp[pos-1] == '0') { tmp[pos++] = '0' + c / 10; c %= 10; }
    tmp[pos++] = '0' + c;
    tmp[pos++] = '.';
    d = ip & 0xFF;
    if (d >= 100) { tmp[pos++] = '0' + d / 100; d %= 100; }
    if (d >= 10 || tmp[pos-1] == '0') { tmp[pos++] = '0' + d / 10; d %= 10; }
    tmp[pos++] = '0' + d;
    tmp[pos] = 0;
    strcpy(buf, tmp);
}

int main(void) {
    puts("=== AuraLite HTTP Client ===");
    puts("Type a hostname to fetch (e.g. example.com)");
    puts("Or type 'quit' to exit.");
    puts("");

    for (;;) {
        write(1, "http> ", 6);
        int64_t n = read(0, hostname, sizeof(hostname) - 1);
        if (n <= 0) continue;
        hostname[n] = 0;
        if (n > 0 && hostname[n-1] == '\n') hostname[n-1] = 0;
        if (hostname[0] == 0) continue;

        if (strcmp(hostname, "quit") == 0 || strcmp(hostname, "q") == 0) break;

        /* Resolve the hostname via DNS. */
        printf("Resolving %s...\n", hostname);
        uint32_t ip = dns_resolve(hostname);
        if (ip == 0) {
            printf("Failed to resolve %s\n", hostname);
            continue;
        }

        char ipstr[20];
        ip_to_str(ip, ipstr);
        printf("Connecting to %s (%s) port 80...\n", hostname, ipstr);

        /* Connect via TCP. */
        if (net_connect(ip, 80) != 0) {
            puts("Connection failed!");
            continue;
        }
        puts("Connected! Sending HTTP request...");

        /* Build HTTP GET request. */
        int req_len = 0;
        const char *get = "GET / HTTP/1.0\r\nHost: ";
        for (int i = 0; get[i]; i++) request[req_len++] = get[i];
        for (int i = 0; hostname[i]; i++) request[req_len++] = hostname[i];
        request[req_len++] = '\r';
        request[req_len++] = '\n';
        request[req_len++] = '\r';
        request[req_len++] = '\n';

        /* Send the request. */
        int sent = net_send(request, req_len);
        printf("Sent %d bytes. Waiting for response...\n\n", sent);

        /* Receive the response. */
        int total = 0;
        for (;;) {
            int got = net_recv(response + total,
                              sizeof(response) - total - 1);
            if (got <= 0) break;
            total += got;
            if (total >= (int)sizeof(response) - 1) break;
        }
        response[total] = 0;

        if (total > 0) {
            printf("--- Response (%d bytes) ---\n", total);
            /* Print up to 1000 characters of the response. */
            int show = total < 1000 ? total : 1000;
            write(1, response, show);
            if (total > 1000) {
                printf("\n... (%d more bytes truncated)\n", total - 1000);
            }
            puts("\n--- End ---");
        } else {
            puts("No response received.");
        }

        /* Close the connection. */
        net_close();
        puts("");
    }

    puts("Goodbye from HTTP client!");
    return 0;
}
