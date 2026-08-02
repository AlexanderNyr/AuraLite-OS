/*
 * tcpserver.c — minimal HTTP server for AuraLite OS (H5).
 *
 * Binds to port 8080, listens for incoming connections, accepts a client,
 * reads the request, and sends back an HTTP 200 OK response.
 */

#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/socket.h"
#include "netinet/in.h"

int main(void) {
    puts("=== AuraLite TCP HTTP Server ===");
    int serv_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (serv_sock < 0) {
        puts("FAIL: socket() failed");
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        puts("FAIL: bind() failed");
        closesocket(serv_sock);
        return 1;
    }
    puts("SERVER: bind(port 8080) OK");

    if (listen(serv_sock, 5) < 0) {
        puts("FAIL: listen() failed");
        closesocket(serv_sock);
        return 1;
    }
    puts("SERVER: listen() OK, waiting for connection...");

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(serv_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock < 0) {
        puts("FAIL: accept() failed / timeout");
        closesocket(serv_sock);
        return 1;
    }

    printf("SERVER: accepted connection from IP 0x%08x port %d\n",
           ntohl(client_addr.sin_addr.s_addr), ntohs(client_addr.sin_port));

    char buf[1024];
    int n = recv(client_sock, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("SERVER: received %d bytes: %s\n", n, buf);
    }

    const char *response = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nHello from AuraLite TCP Server!";
    int sent = send(client_sock, response, strlen(response));
    printf("SERVER: sent %d bytes of HTTP response\n", sent);

    closesocket(client_sock);
    closesocket(serv_sock);
    puts("SERVER: finished successfully");
    return 0;
}
