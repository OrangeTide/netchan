/* udp_echo.c : trivial UDP echo server, to isolate the gateway relay in the
 * headless test (stands in for game_server). Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

int
main(int argc, char **argv)
{
    uint16_t port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 9000;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    printf("udp echo on %d\n", port);
    for (;;) {
        uint8_t buf[2048];
        struct sockaddr_storage from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n > 0)
            sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&from, fl);
    }
}
