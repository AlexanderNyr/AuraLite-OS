/*
 * http.c — minimal HTTP client for AuraLite OS.
 *
 * Performs a raw HTTP/1.0 GET request over TCP and prints the response.
 * Uses the kernel's TCP stack via syscalls.
 *
 * Note: actual TCP syscalls are not yet exposed to userspace, so this
 * program demonstrates the intended interface and prints a status message.
 * Once tcp_connect/tcp_send/tcp_recv syscalls are added, this will do
 * real HTTP requests.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(void) {
    puts("AuraLite HTTP Client");
    puts("");
    puts("HTTP client requires TCP syscalls (not yet wired to userspace).");
    puts("The kernel TCP stack is verified working — see boot log:");
    puts("  [tcp] PASS: TCP connect + send + close all worked");
    puts("");
    puts("Once SYS_TCP_CONNECT / SYS_TCP_SEND / SYS_TCP_RECV are added,");
    puts("this program will fetch real web pages over HTTP.");
    puts("");
    puts("Example usage: http http://example.com/");
    puts("");

    /* Just demonstrate we can run. */
    puts("HTTP client stub complete.");
    return 0;
}
