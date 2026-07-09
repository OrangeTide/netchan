/* rtc_gateway.c : relay browser WebRTC data-channel clients onto the UDP
 * game server, using libpeer for the WebRTC end. The nc_rtc counterpart of
 * ws_gateway: same idea (terminate a browser transport, relay each datagram
 * to an unmodified UDP game_server as its own peer), different pipe.
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 *
 * Signaling is a single HTTP POST /offer -> answer, because libpeer gathers
 * ICE non-trickle: the answer SDP already carries every candidate, so one
 * request/response is the whole handshake. GET serves the demo page.
 *
 * Each browser gets its own thread: libpeer's peer_connection_loop blocks
 * during the DTLS handshake, so peers cannot share a thread. That thread also
 * owns a UDP socket to the game server; datagrams flow
 *   browser --dc--> onmessage --> sendto(udp) --> game_server
 *   game_server --> recv(udp) --> datachannel_send --dc--> browser
 *
 *   rtc_gateway [http_port] [game_host] [game_port] [docroot]
 *   defaults:    8090       127.0.0.1   9000        web
 */

#include "peer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_CLIENTS 8
#define DGRAM_MAX   2048

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static struct sockaddr_in g_server;   /* the UDP game server */

struct client {
    PeerConnection  *pc;
    int              udp_fd;           /* our socket toward the game server */
    pthread_t        thread;
    volatile int     state;           /* PeerConnectionState */
    volatile int     used;
};

static struct client clients[MAX_CLIENTS];

/* ------- libpeer callbacks (run on the client's own thread) ------- */

static void
on_state(PeerConnectionState st, void *user)
{
    struct client *c = user;
    c->state = st;
}

/* datagram arrived from the browser over the data channel: forward to the
 * game server verbatim. */
static void
on_message(char *msg, size_t len, void *user, uint16_t sid)
{
    (void)sid;
    struct client *c = user;
    if (len > 0 && len <= DGRAM_MAX)
        send(c->udp_fd, msg, len, 0);
}

static void on_open(void *user)  { (void)user; }
static void on_close(void *user) { (void)user; }

/* Per-client driver: pump libpeer and relay the server's UDP replies back
 * onto the data channel. */
static void *
client_thread(void *arg)
{
    struct client *c = arg;
    for (;;) {
        peer_connection_loop(c->pc);

        /* drain any datagrams the game server sent us */
        for (;;) {
            uint8_t dg[DGRAM_MAX];
            ssize_t n = recv(c->udp_fd, dg, sizeof(dg), MSG_DONTWAIT);
            if (n <= 0)
                break;
            if (c->state == PEER_CONNECTION_COMPLETED)
                peer_connection_datachannel_send(c->pc, (char *)dg, (size_t)n);
        }

        if (!running || c->state == PEER_CONNECTION_CLOSED ||
            c->state == PEER_CONNECTION_FAILED)
            break;
        usleep(1000);
    }
    return NULL;
}

static int
udp_to_server(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&g_server, sizeof(g_server)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------- HTTP signaling + static files ------- */

static void
send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        break;
    }
}

static const char *
content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    return "application/octet-stream";
}

static void
serve_static(int fd, const char *urlpath, const char *docroot)
{
    if (strstr(urlpath, "..")) { send_all(fd, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n", 44); return; }
    const char *rel = strcmp(urlpath, "/") ? urlpath : "/play-rtc.html";
    char full[1024];
    snprintf(full, sizeof(full), "%s%s", docroot, rel);
    FILE *f = fopen(full, "rb");
    if (!f) { send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 45); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
        "Connection: close\r\n\r\n", content_type(full), sz);
    send_all(fd, hdr, (size_t)hn);
    uint8_t buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) send_all(fd, buf, got);
    fclose(f);
}

/* Accept an SDP offer in the POST body, create a PeerConnection answerer, and
 * return the answer SDP. Spawns the client's driver thread. */
static void
handle_offer(int fd, const char *body)
{
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!clients[i].used) { slot = i; break; }
    if (slot < 0) { send_all(fd, "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\n\r\n", 40); return; }

    struct client *c = &clients[slot];
    memset(c, 0, sizeof(*c));
    c->udp_fd = udp_to_server();
    if (c->udp_fd < 0) { send_all(fd, "HTTP/1.1 500 no udp\r\nContent-Length: 0\r\n\r\n", 42); return; }

    PeerConfiguration cfg = {
        .datachannel = DATA_CHANNEL_BINARY,
        .ice_servers = { { .urls = "stun:stun.l.google.com:19302" } },
        .user_data = c,
    };
    c->pc = peer_connection_create(&cfg);
    peer_connection_oniceconnectionstatechange(c->pc, on_state);
    peer_connection_ondatachannel(c->pc, on_message, on_open, on_close);
    peer_connection_set_remote_description(c->pc, body, SDP_TYPE_OFFER);
    const char *answer = peer_connection_create_answer(c->pc);
    if (!answer) {
        close(c->udp_fd);
        peer_connection_destroy(c->pc);
        send_all(fd, "HTTP/1.1 500 no answer\r\nContent-Length: 0\r\n\r\n", 45);
        return;
    }

    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/sdp\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(answer));
    send_all(fd, hdr, (size_t)hn);
    send_all(fd, answer, strlen(answer));

    c->used = 1;
    pthread_create(&c->thread, NULL, client_thread, c);
    printf("rtc_gateway: browser joined (slot %d)\n", slot);
}

static void
handle_conn(int fd, const char *docroot)
{
    char req[8192];
    size_t n = 0;
    /* read request headers (and any body already arrived) */
    while (n < sizeof(req) - 1) {
        ssize_t r = recv(fd, req + n, sizeof(req) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        req[n] = '\0';
        char *hend = strstr(req, "\r\n\r\n");
        if (!hend) continue;
        if (strncmp(req, "POST", 4) == 0) {
            /* ensure the whole body is present (Content-Length) */
            size_t hlen = (size_t)(hend + 4 - req);
            const char *cl = strcasestr(req, "Content-Length:");
            size_t want = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
            while (n - hlen < want && n < sizeof(req) - 1) {
                ssize_t r2 = recv(fd, req + n, sizeof(req) - 1 - n, 0);
                if (r2 <= 0) break;
                n += (size_t)r2; req[n] = '\0';
            }
            handle_offer(fd, hend + 4);
        } else {
            char path[512] = "/";
            sscanf(req, "GET %511s", path);
            serve_static(fd, path, docroot);
        }
        break;
    }
    close(fd);
}

int
main(int argc, char **argv)
{
    uint16_t http_port = (argc > 1) ? (uint16_t)atoi(argv[1]) : 8090;
    const char *host   = (argc > 2) ? argv[2] : "127.0.0.1";
    uint16_t g_port    = (argc > 3) ? (uint16_t)atoi(argv[3]) : 9000;
    const char *docroot = (argc > 4) ? argv[4] : "web";

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    memset(&g_server, 0, sizeof(g_server));
    g_server.sin_family = AF_INET;
    g_server.sin_port = htons(g_port);
    if (inet_pton(AF_INET, host, &g_server.sin_addr) != 1) {
        fprintf(stderr, "rtc_gateway: bad game host '%s'\n", host);
        return 1;
    }

    peer_init();

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(http_port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    listen(lfd, 8);
    printf("rtc gateway on http://0.0.0.0:%d  ->  udp %s:%d  (docroot '%s')\n",
           http_port, host, g_port, docroot);
    printf("POST an SDP offer to /offer; GET / for the demo page\n");

    while (running) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        handle_conn(cfd, docroot);
    }

    printf("\nrtc gateway shutting down\n");
    running = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].used) {
            pthread_join(clients[i].thread, NULL);
            peer_connection_close(clients[i].pc);
            peer_connection_destroy(clients[i].pc);
            close(clients[i].udp_fd);
        }
    close(lfd);
    peer_deinit();
    return 0;
}
