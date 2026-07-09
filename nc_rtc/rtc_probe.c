/* rtc_probe.c : browser-equivalent WebRTC client for testing rtc_gateway
 * without a browser. It is the libpeer offerer: POSTs an SDP offer to the
 * gateway, opens a data channel, sends one datagram, and checks that the
 * gateway relayed it to the UDP server and the reply back. A green run proves
 * the whole nc_rtc relay path. Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 *
 *   rtc_probe [gateway_host] [gateway_http_port]
 */

#include "peer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static PeerConnection *pc;
static volatile int state, dc_created, got_echo, stop;
static const char *PAYLOAD = "PING-over-webrtc";

static void
on_state(PeerConnectionState s, void *u)
{
    (void)u;
    state = s;
    if (s == PEER_CONNECTION_COMPLETED && !dc_created) {
        peer_connection_create_datachannel(pc, DATA_CHANNEL_RELIABLE, 0, 0, "game", "");
        dc_created = 1;
    }
}

static void
on_open(void *u)
{
    (void)u;
    printf("probe: data channel open, sending %zu bytes\n", strlen(PAYLOAD));
    peer_connection_datachannel_send(pc, (char *)PAYLOAD, strlen(PAYLOAD));
}

static void
on_message(char *msg, size_t len, void *u, uint16_t sid)
{
    (void)u; (void)sid;
    printf("probe: received %.*s\n", (int)len, msg);
    if (len == strlen(PAYLOAD) && memcmp(msg, PAYLOAD, len) == 0)
        got_echo = 1;
}

static void *
pc_thread(void *arg)
{
    (void)arg;
    while (!stop) { peer_connection_loop(pc); usleep(1000); }
    return NULL;
}

/* POST the offer to the gateway, return the answer SDP (malloc'd) or NULL. */
static char *
signal_offer(const char *host, uint16_t port, const char *offer)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return NULL; }

    char req[8192];
    int rn = snprintf(req, sizeof(req),
        "POST /offer HTTP/1.1\r\nHost: %s\r\nContent-Type: application/sdp\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        host, strlen(offer), offer);
    if (send(fd, req, (size_t)rn, 0) != rn) { close(fd); return NULL; }

    char resp[8192];
    size_t n = 0;
    for (;;) {
        ssize_t r = recv(fd, resp + n, sizeof(resp) - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (n >= sizeof(resp) - 1) break;
    }
    close(fd);
    resp[n] = '\0';
    char *body = strstr(resp, "\r\n\r\n");
    if (!body || strstr(resp, "200") == NULL) return NULL;
    return strdup(body + 4);
}

int
main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8090;

    peer_init();
    PeerConfiguration cfg = { .datachannel = DATA_CHANNEL_BINARY };
    pc = peer_connection_create(&cfg);
    peer_connection_oniceconnectionstatechange(pc, on_state);
    peer_connection_ondatachannel(pc, on_message, on_open, NULL);

    char *offer = strdup(peer_connection_create_offer(pc));
    char *answer = signal_offer(host, port, offer);
    if (!answer) { fprintf(stderr, "probe: signaling failed\n"); return 1; }
    peer_connection_set_remote_description(pc, answer, SDP_TYPE_ANSWER);

    pthread_t t;
    pthread_create(&t, NULL, pc_thread, NULL);
    for (int i = 0; i < 1500 && !got_echo; i++)
        usleep(10000);                     /* up to 15 s */
    stop = 1;
    pthread_join(t, NULL);

    printf("probe: final state=%s\n", peer_connection_state_to_string(state));
    free(offer); free(answer);
    peer_connection_close(pc);
    peer_connection_destroy(pc);
    peer_deinit();

    if (got_echo) { printf("rtc_probe: PASS (datagram relayed through the gateway)\n"); return 0; }
    printf("rtc_probe: FAIL (no echo)\n");
    return 1;
}
