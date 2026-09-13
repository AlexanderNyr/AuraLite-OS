/* w32/tests/w32a2_pipes.c — W32A-2 guest fixture: pipes.
 *
 * Anonymous pipe round-trips plus the named-pipe rendezvous over the VFS
 * fifo layer: single-instance semantics, the connected/not-connected
 * handshake errors, the message-mode refusal, and WaitNamedPipe hit and
 * miss.  Exits 55/1.
 */

#include "w32a2_common.h"

void __stdcall winstart(void) {
    HANDLE r, wr, srv, cli;
    DWORD n;
    char buf[32];

    w32a2_out = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Anonymous pipe. */
    CHECKX(CreatePipe(&r, &wr, NULL, 0), "pipe-create");
    CHECKX(WriteFile(wr, "ping", 4, &n, NULL) && n == 4, "pipe-write");
    CHECKX(ReadFile(r, buf, 4, &n, NULL) && n == 4, "pipe-read");
    CHECKX(memcmp(buf, "ping", 4) == 0, "pipe-data");
    CHECKX(GetFileType(r) == FILE_TYPE_PIPE, "pipe-type");
    CHECKX(CancelIo(r), "pipe-cancel");
    CHECKX(CloseHandle(r), "pipe-close-r");
    CHECKX(CloseHandle(wr), "pipe-close-w");

    /* Named pipe rendezvous: server, then client, then connect. */
    srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2t", PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
        5000, NULL);
    CHECKX(srv != INVALID_HANDLE_VALUE, "npipe-create");
    if (srv == INVALID_HANDLE_VALUE) {
        w32a2_done("PIPES");
        return;
    }
    CHECKX(!ConnectNamedPipe(srv, NULL), "npipe-connect-early-fails");
    CHECKX(GetLastError() == ERROR_PIPE_NOT_CONNECTED,
        "npipe-connect-early-code");
    cli = CreateFileW(L"\\\\.\\pipe\\w32a2t", GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    CHECKX(cli != INVALID_HANDLE_VALUE, "npipe-client");
    if (cli != INVALID_HANDLE_VALUE) {
        CHECKX(!ConnectNamedPipe(srv, NULL), "npipe-connect-fails");
        CHECKX(GetLastError() == ERROR_PIPE_CONNECTED,
            "npipe-connect-code");
        CHECKX(WriteFile(cli, "req", 3, &n, NULL) && n == 3,
            "npipe-write");
        CHECKX(ReadFile(srv, buf, 3, &n, NULL) && n == 3, "npipe-read");
        CHECKX(memcmp(buf, "req", 3) == 0, "npipe-data");
        CHECKX(WriteFile(srv, "resp", 4, &n, NULL) && n == 4,
            "npipe-write-back");
        CHECKX(ReadFile(cli, buf, 4, &n, NULL) && n == 4, "npipe-read-back");
        CHECKX(memcmp(buf, "resp", 4) == 0, "npipe-data-back");
        CHECKX(CloseHandle(cli), "npipe-client-close");
    }
    CHECKX(CloseHandle(srv), "npipe-close");

    /* A second instance while max is 1 is busy, by name. */
    srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2one", PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE, 1, 0, 0, 0, NULL);
    CHECKX(srv != INVALID_HANDLE_VALUE, "npipe-one");
    if (srv != INVALID_HANDLE_VALUE) {
        HANDLE srv2 = CreateNamedPipeA("\\\\.\\pipe\\w32a2one",
            PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE, 1, 0, 0, 0, NULL);
        CHECKX(srv2 == INVALID_HANDLE_VALUE, "npipe-busy-handle");
        CHECKX(GetLastError() == ERROR_PIPE_BUSY, "npipe-busy-code");
        CHECKX(CloseHandle(srv), "npipe-one-close");
    }

    /* Message mode is refused. */
    CHECKX(CreateNamedPipeA("\\\\.\\pipe\\w32a2msg", PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE, 1, 0, 0, 0, NULL) == INVALID_HANDLE_VALUE,
        "npipe-msg-handle");
    CHECKX(GetLastError() == ERROR_INVALID_PARAMETER, "npipe-msg-code");

    /* WaitNamedPipe: instant hit and instant miss. */
    srv = CreateNamedPipeA("\\\\.\\pipe\\w32a2wait", PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE, 1, 0, 0, 0, NULL);
    CHECKX(srv != INVALID_HANDLE_VALUE, "npipe-wait-create");
    if (srv != INVALID_HANDLE_VALUE) {
        CHECKX(WaitNamedPipeA("\\\\.\\pipe\\w32a2wait", NMPWAIT_NOWAIT),
            "npipe-wait-hit");
        CHECKX(!WaitNamedPipeA("\\\\.\\pipe\\w32a2nobody", NMPWAIT_NOWAIT),
            "npipe-wait-miss");
        CHECKX(GetLastError() == ERROR_PIPE_BUSY, "npipe-wait-code");
        CHECKX(CloseHandle(srv), "npipe-wait-close");
    }

    w32a2_done("PIPES");
}
