/* netchan_example.c : chat server + client over localhost UDP */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */
/*
 * Demonstrates netchan with real sockets: one server, up to 4 clients.
 *
 * Build:
 *   cc -Wall -W -o netchan_example netchan_example.c netchan.c
 *
 * Run (each in a separate terminal):
 *   ./netchan_example server
 *   ./netchan_example client Alice
 *   ./netchan_example client Bob
 *
 * Type messages in any client terminal. They appear on all other clients.
 * Ctrl-C or "quit" to disconnect.
 */

#include "netchan.h"
#include "nc_udp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define SERVER_PORT 9900
#define MAX_PEERS   4
#define TICK_MS     50
#define MSG_MAX     512

static volatile sig_atomic_t running = 1;

static void
handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

/* Return monotonic time in milliseconds. */
static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Create a non-blocking UDP socket bound to the given port (0 for any). */
static int
udp_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    return fd;
}

/* Send all pending netchan packets out through the UDP socket. */
static void
flush_sends(int fd, struct netchan_conn *conn)
{
    uint8_t buf[2048];
    struct nc_addr dst = {0};

    for (;;) {
        size_t n = netchan_send_next(conn, buf, sizeof(buf), &dst);
        if (n == 0) break;
        struct sockaddr_storage ss;
        socklen_t sl = nc_udp_to_sockaddr(&dst, &ss);
        if (sl == 0) continue;
        sendto(fd, buf, n, 0, (struct sockaddr *)&ss, sl);
    }
}

/*-------------------------------------------------------------
 * Server: accept up to MAX_PEERS clients, broadcast messages
 *-------------------------------------------------------------*/

struct peer {
    struct netchan_conn *conn;
    struct netchan_chan  *recv_ch; /* their send channel (our receive) */
    struct netchan_chan  *send_ch; /* our send channel to them */
    char name[MSG_MAX];
    int  active;
};

static void
server_broadcast(int fd, struct peer peers[], int from,
                 const char *text, size_t len)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!peers[i].active || !peers[i].send_ch || i == from)
            continue;
        netchan_chan_write(peers[i].send_ch, text, len);
        flush_sends(fd, peers[i].conn);
    }
}

static void
run_server(void)
{
    int fd = udp_socket(SERVER_PORT);
    printf("server listening on 127.0.0.1:%d (max %d peers)\n",
           SERVER_PORT, MAX_PEERS);

    struct peer peers[MAX_PEERS] = {{0}};

    while (running) {
        /* poll the socket with a short timeout for the tick loop */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ret = poll(&pfd, 1, TICK_MS);

        if (ret > 0 && (pfd.revents & POLLIN)) {
            uint8_t pkt[2048];
            struct sockaddr_storage from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n <= 0) goto service;

            struct nc_addr naddr;
            nc_udp_from_sockaddr(&naddr, (struct sockaddr *)&from, fromlen);

            /*
             * Demux: peek the connection ID to find which peer
             * this packet belongs to. ID 0 means a new connection.
             */
            uint32_t id = netchan_peek_id(pkt, (size_t)n);
            int slot = -1;

            for (int i = 0; i < MAX_PEERS; i++) {
                if (peers[i].active && netchan_id(peers[i].conn) == id) {
                    slot = i;
                    break;
                }
            }

            if (slot >= 0) {
                /* existing peer */
                netchan_feed(peers[slot].conn, pkt, (size_t)n, &naddr);
            } else {
                /* new connection attempt: find a free slot */
                for (int i = 0; i < MAX_PEERS; i++) {
                    if (!peers[i].active) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    printf("server: full, rejecting connection\n");
                    goto service;
                }
                peers[slot].conn = netchan_open(1);
                netchan_feed(peers[slot].conn, pkt, (size_t)n, &naddr);
                netchan_accept(peers[slot].conn);
                peers[slot].active = 1;
                snprintf(peers[slot].name, sizeof(peers[slot].name),
                         "peer-%d", slot);
                printf("server: peer %d connecting\n", slot);
                flush_sends(fd, peers[slot].conn);
            }
        }

service:
        /* service timers and process events for all peers */
        for (int i = 0; i < MAX_PEERS; i++) {
            if (!peers[i].active) continue;

            netchan_service(peers[i].conn, now_ms());
            flush_sends(fd, peers[i].conn);

            struct netchan_event ev;
            while (netchan_poll(peers[i].conn, &ev)) {
                switch (ev.type) {
                case NETCHAN_EV_CONNECTED:
                    printf("server: peer %d connected\n", i);
                    /* open a send channel to push broadcast messages */
                    peers[i].send_ch = netchan_chan_open(
                        peers[i].conn, NETCHAN_RELIABLE,
                        NETCHAN_DIR_SEND, "chat");
                    flush_sends(fd, peers[i].conn);
                    break;

                case NETCHAN_EV_CHAN_OPEN:
                    /* the client opened a channel; use it to receive */
                    peers[i].recv_ch = ev.ch;
                    break;

                case NETCHAN_EV_DATA: {
                    char buf[MSG_MAX];
                    int rd = netchan_chan_read(ev.ch, buf, sizeof(buf) - 1);
                    if (rd <= 0) break;
                    buf[rd] = '\0';

                    /* first message is the client's name */
                    if (strcmp(peers[i].name, buf) != 0 &&
                        peers[i].name[0] == 'p') {
                        snprintf(peers[i].name, sizeof(peers[i].name),
                                 "%s", buf);
                        printf("server: peer %d is \"%s\"\n", i,
                               peers[i].name);
                        break;
                    }

                    printf("[%s] %s\n", peers[i].name, buf);

                    /* format and broadcast to other peers */
                    char out[MSG_MAX];
                    int outlen = snprintf(out, sizeof(out), "[%s] %s",
                                          peers[i].name, buf);
                    server_broadcast(fd, peers, i, out, (size_t)outlen);
                    break;
                }

                case NETCHAN_EV_DISCONNECTED:
                    printf("server: peer %d (\"%s\") disconnected\n",
                           i, peers[i].name);
                    netchan_close(peers[i].conn);
                    memset(&peers[i], 0, sizeof(peers[i]));
                    break;
                }
            }

            /* periodically print stats */
            if (peers[i].active) {
                struct netchan_conn_stats cs;
                netchan_conn_stats(peers[i].conn, &cs);
                /* stats available: cs.rtt_ms, cs.pkts_sent, etc. */
                (void)cs;
            }
        }
    }

    /* clean up */
    for (int i = 0; i < MAX_PEERS; i++) {
        if (peers[i].active)
            netchan_close(peers[i].conn);
    }
    close(fd);
    printf("server: shutdown\n");
}

/*-------------------------------------------------------------
 * Client: connect, open a chat channel, read from stdin
 *-------------------------------------------------------------*/

static void
run_client(const char *name)
{
    int fd = udp_socket(0);

    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = htons(SERVER_PORT);

    struct nc_addr server_naddr;
    nc_udp_from_sockaddr(&server_naddr, (struct sockaddr *)&saddr,
                         sizeof(saddr));

    struct netchan_conn *conn = netchan_open(0);
    netchan_connect(conn, &server_naddr);
    flush_sends(fd, conn);

    printf("connecting as \"%s\"...\n", name);

    struct netchan_chan *send_ch = NULL;
    struct netchan_chan *recv_ch = NULL;
    int connected = 0;

    while (running) {
        /*
         * Poll both the UDP socket and stdin. The socket carries
         * netchan packets; stdin carries user-typed chat messages.
         */
        struct pollfd pfds[2];
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = STDIN_FILENO;
        pfds[1].events = connected ? POLLIN : 0;

        poll(pfds, 2, TICK_MS);

        /* read incoming UDP packets */
        if (pfds[0].revents & POLLIN) {
            uint8_t pkt[2048];
            struct sockaddr_storage from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(fd, pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                struct nc_addr naddr;
                nc_udp_from_sockaddr(&naddr, (struct sockaddr *)&from,
                                     fromlen);
                netchan_feed(conn, pkt, (size_t)n, &naddr);
            }
        }

        netchan_service(conn, now_ms());
        flush_sends(fd, conn);

        /* process events */
        struct netchan_event ev;
        while (netchan_poll(conn, &ev)) {
            switch (ev.type) {
            case NETCHAN_EV_CONNECTED:
                printf("connected!\n");
                connected = 1;

                /* open a reliable channel for sending chat messages */
                send_ch = netchan_chan_open(conn, NETCHAN_RELIABLE,
                                           NETCHAN_DIR_SEND, "chat");
                flush_sends(fd, conn);

                /* send our name as the first message */
                netchan_chan_write(send_ch, name, strlen(name));
                flush_sends(fd, conn);
                break;

            case NETCHAN_EV_CHAN_OPEN:
                /* the server opened a channel to us for receiving */
                recv_ch = ev.ch;
                break;

            case NETCHAN_EV_DATA: {
                char buf[MSG_MAX];
                int rd = netchan_chan_read(ev.ch, buf, sizeof(buf) - 1);
                if (rd > 0) {
                    buf[rd] = '\0';
                    printf("%s\n", buf);
                }
                break;
            }

            case NETCHAN_EV_DISCONNECTED:
                printf("disconnected by server\n");
                running = 0;
                break;
            }
        }

        flush_sends(fd, conn);

        /* read a line from stdin and send it */
        if (connected && send_ch && (pfds[1].revents & POLLIN)) {
            char line[MSG_MAX];
            if (fgets(line, sizeof(line), stdin)) {
                /* strip newline */
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n')
                    line[--len] = '\0';
                if (len == 0) continue;

                if (strcmp(line, "quit") == 0) {
                    running = 0;
                    break;
                }

                /* print stats inline with the message */
                struct netchan_conn_stats cs;
                netchan_conn_stats(conn, &cs);
                printf("(rtt %ums, %u sent, %u recv) > %s\n",
                       cs.rtt_ms, cs.pkts_sent, cs.pkts_recv, line);

                netchan_chan_write(send_ch, line, len);
                flush_sends(fd, conn);
            }
        }
    }

    netchan_close(conn);
    close(fd);
    (void)recv_ch;
}

/*-------------------------------------------------------------
 * Main
 *-------------------------------------------------------------*/

int
main(int argc, char **argv)
{
    signal(SIGINT, handle_sigint);

    if (argc < 2) {
        fprintf(stderr, "usage: %s server\n", argv[0]);
        fprintf(stderr, "       %s client <name>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "server") == 0) {
        run_server();
    } else if (strcmp(argv[1], "client") == 0) {
        const char *name = (argc > 2) ? argv[2] : "anon";
        run_client(name);
    } else {
        fprintf(stderr, "unknown mode: %s\n", argv[1]);
        return 1;
    }
    return 0;
}
