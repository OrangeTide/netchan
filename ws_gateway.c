/* ws_gateway.c : relay browser WebSocket clients onto the UDP game server */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The game server is pure UDP and knows nothing about browsers. This gateway
 * bridges the gap: it terminates a WebSocket connection from a browser and
 * relays each binary message, unchanged, as a UDP datagram to the server, and
 * relays the server's UDP replies back as binary messages. Because netchan's
 * datagram boundary is preserved by the one-message-per-packet rule, the
 * protocol core on both ends is oblivious to the detour.
 *
 * Each browser gets its own UDP socket toward the server, so on the server it
 * simply looks like one more UDP peer sitting next to the native clients. That
 * is the whole point: a browser player and a terminal player share one
 * unmodified server at the same time.
 *
 * The same listener also answers plain HTTP GETs from a document root, so the
 * demo page and its wasm serve from this one binary with no second web server.
 *
 *   ws_gateway [ws_port] [game_host] [game_port] [docroot]
 *   defaults:   8080      127.0.0.1   9000        web
 */

#include "nc_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_CLIENTS  16
#define BUFCAP       8192
#define DGRAM_MAX    2048

enum cstate { CS_FREE = 0, CS_HANDSHAKE, CS_OPEN };

struct client {
    enum cstate state;
    int      ws_fd;                 /* TCP to the browser */
    int      udp_fd;                /* our UDP socket toward the game server */
    uint8_t  rx[BUFCAP];            /* bytes read from the browser, unparsed */
    size_t   rxn;
};

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static void
set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int
listen_tcp(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    if (listen(fd, 8) < 0) { perror("listen"); exit(1); }
    set_nonblock(fd);
    return fd;
}

/* A UDP socket connect()ed to the game server, so send/recv need no address
 * and the socket only ever sees that server's replies. */
static int
udp_to_server(const struct sockaddr_in *srv)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (const struct sockaddr *)srv, sizeof(*srv)) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

static void
client_close(struct client *c)
{
    if (c->ws_fd >= 0) close(c->ws_fd);
    if (c->udp_fd >= 0) close(c->udp_fd);
    memset(c, 0, sizeof(*c));
    c->ws_fd = c->udp_fd = -1;
}

/****************************************************************
 * minimal static file server (for the demo page + wasm)
 ****************************************************************/

static const char *
content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    if (!strcmp(dot, ".css"))  return "text/css";
    return "application/octet-stream";
}

static void
send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;                       /* peer gone; give up on this file */
    }
}

static void
http_error(int fd, const char *status)
{
    char h[128];
    int n = snprintf(h, sizeof(h),
        "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    if (n > 0)
        send_all(fd, h, (size_t)n);
}

/* Serve one file named by the request line, rooted at docroot. This is a demo
 * helper, not a hardened web server, but it does refuse paths containing "..". */
static void
serve_static(int fd, const char *req, const char *docroot)
{
    if (strncmp(req, "GET ", 4) != 0) { http_error(fd, "405 Method Not Allowed"); return; }
    const char *p = req + 4;
    const char *end = p;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n')
        end++;
    char urlpath[512];
    size_t ulen = (size_t)(end - p);
    if (ulen == 0 || ulen >= sizeof(urlpath)) { http_error(fd, "400 Bad Request"); return; }
    memcpy(urlpath, p, ulen);
    urlpath[ulen] = '\0';
    if (strstr(urlpath, "..")) { http_error(fd, "403 Forbidden"); return; }

    const char *rel = urlpath;
    if (!strcmp(rel, "/")) rel = "/play.html";

    char full[1024];
    if ((size_t)snprintf(full, sizeof(full), "%s%s", docroot, rel) >= sizeof(full)) {
        http_error(fd, "414 URI Too Long");
        return;
    }
    FILE *f = fopen(full, "rb");
    if (!f) { http_error(fd, "404 Not Found"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); http_error(fd, "500 Internal Server Error"); return; }

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
        "Connection: close\r\n\r\n", content_type(full), sz);
    if (hn > 0)
        send_all(fd, hdr, (size_t)hn);

    uint8_t chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
        send_all(fd, chunk, got);
    fclose(f);
}

/****************************************************************
 * relay
 ****************************************************************/

/* Read available bytes from the browser into c->rx. Returns 0 to keep the
 * client, -1 if it should be dropped. */
static int
pump_ws_in(struct client *c, const struct sockaddr_in *srv, const char *docroot)
{
    if (c->rxn >= sizeof(c->rx))
        return -1;                   /* client is flooding without framing */
    ssize_t n = recv(c->ws_fd, c->rx + c->rxn, sizeof(c->rx) - c->rxn, 0);
    if (n == 0)
        return -1;                   /* orderly close */
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    c->rxn += (size_t)n;

    if (c->state == CS_HANDSHAKE) {
        char resp[256];
        int r = nc_ws_accept((const char *)c->rx, c->rxn, resp, sizeof(resp));
        if (r > 0) {
            send_all(c->ws_fd, resp, (size_t)r);
            c->rxn = 0;
            c->udp_fd = udp_to_server(srv);
            if (c->udp_fd < 0)
                return -1;
            c->state = CS_OPEN;
            return 0;
        }
        if (r == 0)
            return 0;                /* need more of the request */
        /* Not a WebSocket upgrade: treat as a plain file request and close. */
        serve_static(c->ws_fd, (const char *)c->rx, docroot);
        return -1;
    }

    /* CS_OPEN: parse as many whole frames as have arrived. */
    for (;;) {
        struct nc_ws_frame f;
        long used = nc_ws_frame_parse(c->rx, c->rxn, &f);
        if (used == 0)
            break;                   /* partial frame, wait for more */
        if (used < 0)
            return -1;               /* protocol error */
        if (f.opcode == NC_WS_BINARY && f.payload_len > 0 &&
            f.payload_len <= DGRAM_MAX) {
            send(c->udp_fd, f.payload, f.payload_len, 0);
        } else if (f.opcode == NC_WS_CLOSE) {
            return -1;
        } else if (f.opcode == NC_WS_PING) {
            uint8_t pong[DGRAM_MAX + 16];
            size_t pn = nc_ws_frame_build(pong, sizeof(pong), NC_WS_PONG,
                                          f.payload, f.payload_len, NULL);
            if (pn > 0)
                send_all(c->ws_fd, pong, pn);
        }
        memmove(c->rx, c->rx + used, c->rxn - (size_t)used);
        c->rxn -= (size_t)used;
    }
    return 0;
}

/* Drain the server's UDP replies and frame them back to the browser. */
static int
pump_udp_in(struct client *c)
{
    for (;;) {
        uint8_t dg[DGRAM_MAX];
        ssize_t n = recv(c->udp_fd, dg, sizeof(dg), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return 0;
            return (n == 0) ? 0 : -1;
        }
        uint8_t frame[DGRAM_MAX + 16];
        size_t fl = nc_ws_frame_build(frame, sizeof(frame), NC_WS_BINARY,
                                      dg, (size_t)n, NULL);
        if (fl > 0)
            send_all(c->ws_fd, frame, fl);
    }
}

int
main(int argc, char **argv)
{
    uint16_t ws_port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 8080;
    const char *host = (argc > 2) ? argv[2] : "127.0.0.1";
    uint16_t g_port  = (argc > 3) ? (uint16_t)atoi(argv[3]) : 9000;
    const char *docroot = (argc > 4) ? argv[4] : "web";

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);        /* a dead browser must not kill us */

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(g_port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) {
        fprintf(stderr, "ws_gateway: bad game host '%s'\n", host);
        return 1;
    }

    int lfd = listen_tcp(ws_port);
    printf("ws gateway on ws://0.0.0.0:%d  ->  udp %s:%d  (docroot '%s')\n",
           ws_port, host, g_port, docroot);

    struct client cs[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) { cs[i].ws_fd = cs[i].udp_fd = -1; }

    while (running) {
        struct pollfd pfd[1 + MAX_CLIENTS * 2];
        int map[1 + MAX_CLIENTS * 2];   /* pfd index -> client index, or -1 */
        int nf = 0;
        pfd[nf].fd = lfd; pfd[nf].events = POLLIN; pfd[nf].revents = 0;
        map[nf] = -1; nf++;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (cs[i].state == CS_FREE)
                continue;
            pfd[nf].fd = cs[i].ws_fd; pfd[nf].events = POLLIN; pfd[nf].revents = 0;
            map[nf] = i; nf++;
            if (cs[i].state == CS_OPEN) {
                pfd[nf].fd = cs[i].udp_fd; pfd[nf].events = POLLIN; pfd[nf].revents = 0;
                map[nf] = i; nf++;
            }
        }

        if (poll(pfd, (nfds_t)nf, 200) <= 0)
            continue;

        if (pfd[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0)
                    break;
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (cs[i].state == CS_FREE) { slot = i; break; }
                if (slot < 0) { close(cfd); continue; }   /* full: refuse */
                set_nonblock(cfd);
                cs[slot].state = CS_HANDSHAKE;
                cs[slot].ws_fd = cfd;
                cs[slot].udp_fd = -1;
                cs[slot].rxn = 0;
            }
        }

        for (int k = 1; k < nf; k++) {
            int i = map[k];
            if (i < 0 || cs[i].state == CS_FREE)
                continue;
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            int drop;
            if (pfd[k].fd == cs[i].ws_fd)
                drop = pump_ws_in(&cs[i], &srv, docroot);
            else
                drop = pump_udp_in(&cs[i]);
            if (drop)
                client_close(&cs[i]);
        }
    }

    printf("\nws gateway shutting down\n");
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (cs[i].state != CS_FREE)
            client_close(&cs[i]);
    close(lfd);
    return 0;
}
