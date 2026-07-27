/* beacon_demo.c : announce a server on the LAN, and browse for one */
/*
 * The two halves of nc_beacon with real sockets under them.
 *
 *   beacon_demo serve  [--port N] [--name S] [--to ADDR[:PORT]] [--every MS]
 *   beacon_demo browse [--port N] [--to ADDR[:PORT]] [--seconds N]
 *
 * A browser binds the discovery port rather than an ephemeral one, because
 * that is where announcements are sent and a socket only hears the port it is
 * bound to. Hosting and playing on one machine therefore means two sockets on
 * one port, which is what SO_REUSEADDR and SO_REUSEPORT are for below.
 *
 * Run the server in one terminal and the browser in another:
 *
 *   ./beacon_demo serve --name "Ashen Coast"
 *   ./beacon_demo browse
 *
 * The server broadcasts every few seconds and answers probes. The browser
 * probes once at startup so it shows something immediately, then sits and
 * listens for the announcements that follow.
 *
 * TWO MACHINES, OR --to
 *
 * Running both halves on one machine with the defaults finds nothing, and the
 * code is not at fault: Linux does not deliver a broadcast back to sockets on
 * the host that sent it. Neither the limited broadcast nor the subnet-directed
 * one comes back. A server and a browser on one desk therefore need a real
 * address between them:
 *
 *   ./beacon_demo serve  --to 127.0.0.1:9902
 *   ./beacon_demo browse --port 9902 --to 127.0.0.1:9901
 *
 * --to earns its place twice over, because broadcast is also the first thing a
 * container or a wireless access point drops. Pointing both sides at loopback
 * exercises every path here without a single broadcast packet, which is how
 * the smoke test runs.
 *
 * WHAT THIS DEMONSTRATES BESIDES THE PACKET
 *
 * nc_beacon builds and parses and does nothing else, so the two rules that
 * keep a probe from becoming a reflector have to live out here. Both are in
 * answer_probe(): the source has to share a subnet with one of this machine's
 * own interfaces, and no address gets answered more than once a second. A
 * server that skips them is a server that will amplify traffic at whoever a
 * stranger names.
 */

#include "nc_beacon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <errno.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* This program has its own failures to report, distinct from the library's. */
#define BD_ERR          (-1)

#define DEFAULT_PORT    9901
#define DEFAULT_EVERY   5000        /* announce interval, milliseconds */
#define GAME_ID         0x5a4f4d42u /* whatever the game picks; "BMOZ" here */
#define REPLY_GAP_MS    1000        /* per-address floor on probe replies */
#define REPLY_SLOTS     16
#define MAX_SEEN        32
#define FORGET_MS       40000       /* drop a server after this much silence */

static volatile sig_atomic_t running = 1;

static void
on_sigint(int sig)
{
    (void)sig;
    running = 0;
}

static uint32_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************
 * Is this peer on one of our own networks?
 *
 * The check that keeps the probe reply a local feature. Every AF_INET
 * interface with a netmask is compared against the source address, so
 * loopback answers loopback and the office subnet answers the office. A
 * stranger on the internet matches nothing and is ignored.
 *
 * IPv6 is left out here only because the demo speaks IPv4. The same rule
 * applies, with the prefix length in place of the mask.
 ****************************************************************/

static int
peer_is_local(const struct sockaddr_in *peer)
{
    struct ifaddrs *list = NULL, *ifa;
    uint32_t want = peer->sin_addr.s_addr;
    int local = 0;

    if (getifaddrs(&list) != 0)
        return 0;               /* cannot tell, so do not answer */

    for (ifa = list; ifa && !local; ifa = ifa->ifa_next) {
        const struct sockaddr_in *a, *m;

        if (!ifa->ifa_addr || !ifa->ifa_netmask ||
            ifa->ifa_addr->sa_family != AF_INET)
            continue;
        a = (const struct sockaddr_in *)(void *)ifa->ifa_addr;
        m = (const struct sockaddr_in *)(void *)ifa->ifa_netmask;
        if ((a->sin_addr.s_addr & m->sin_addr.s_addr) ==
            (want & m->sin_addr.s_addr))
            local = 1;
    }
    freeifaddrs(list);
    return local;
}

/* One reply per address per REPLY_GAP_MS. A ring rather than a real table:
 * losing track of an address costs one extra reply, and a fixed size means a
 * flood cannot make this grow. */
struct limiter {
    struct {
        uint32_t addr;
        uint32_t when;
        int      used;
    } slot[REPLY_SLOTS];
    int next;
};

static int
may_reply(struct limiter *lim, uint32_t addr, uint32_t now)
{
    int i;

    for (i = 0; i < REPLY_SLOTS; i++) {
        if (!lim->slot[i].used || lim->slot[i].addr != addr)
            continue;
        if (now - lim->slot[i].when < REPLY_GAP_MS)
            return 0;
        lim->slot[i].when = now;
        return 1;
    }
    lim->slot[lim->next].addr = addr;
    lim->slot[lim->next].when = now;
    lim->slot[lim->next].used = 1;
    lim->next = (lim->next + 1) % REPLY_SLOTS;
    return 1;
}

/****************************************************************
 * Sockets
 ****************************************************************/

static int
bind_udp(int port, int broadcast)
{
    struct sockaddr_in sa;
    int fd, on = 1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return BD_ERR;

    /* So a browser and a server can sit on the same port on one machine,
     * which is the ordinary case when someone hosts and plays. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    if (broadcast)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return BD_ERR;
    }
    return fd;
}

/* "1.2.3.4" or "1.2.3.4:9901". A missing port means the discovery port. */
static int
parse_to(const char *s, int defport, struct sockaddr_in *out)
{
    char host[64];
    const char *colon = strrchr(s, ':');
    int port = defport;
    size_t n;

    n = colon ? (size_t)(colon - s) : strlen(s);
    if (n == 0 || n >= sizeof(host))
        return BD_ERR;
    memcpy(host, s, n);
    host[n] = '\0';
    if (colon)
        port = atoi(colon + 1);

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, host, &out->sin_addr) == 1 ? 0 : -1;
}

/****************************************************************
 * Serving
 ****************************************************************/

static void
fill_beacon(struct nc_beacon *b, const char *name, int port, uint32_t instance)
{
    memset(b, 0, sizeof(*b));
    b->game = GAME_ID;
    b->wire = 1;
    b->major = 0;
    b->minor = 8;
    b->port = (uint16_t)port;
    b->players = 3;
    b->capacity = 16;
    b->modes = 0x3;
    b->flags = NC_BEACON_F_CREDENTIALS | NC_BEACON_F_REGISTER;
    b->instance = instance;
    snprintf(b->name, sizeof(b->name), "%s", name);
    snprintf(b->contact, sizeof(b->contact), "a demo, not a real server");
}

static void
answer_probe(int fd, const void *pkt, size_t len,
             const struct sockaddr_in *from, struct limiter *lim,
             const struct nc_beacon *self)
{
    uint8_t out[NC_BEACON_MAX];
    uint32_t game = 0;
    long n;

    if (nc_beacon_probe_game(pkt, len, &game) != NC_BEACON_OK)
        return;
    if (game != self->game)
        return;                 /* somebody else's game */

    if (!peer_is_local(from)) {
        printf("probe from %s ignored: not a local address\n",
               inet_ntoa(from->sin_addr));
        return;
    }
    if (!may_reply(lim, from->sin_addr.s_addr, now_ms()))
        return;                 /* answered this address a moment ago */

    n = nc_beacon_build(out, sizeof(out), self);
    if (n > 0)
        sendto(fd, out, (size_t)n, 0, (const struct sockaddr *)from,
               sizeof(*from));
}

static int
serve(int port, const char *name, const struct sockaddr_in *to, int every)
{
    struct nc_beacon self;
    struct limiter lim;
    uint8_t out[NC_BEACON_MAX];
    int fd;
    long n;
    uint32_t next = 0;

    memset(&lim, 0, sizeof(lim));
    fill_beacon(&self, name, port, (uint32_t)getpid());

    fd = bind_udp(port, 1);
    if (fd < 0) {
        fprintf(stderr, "cannot bind port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("announcing \"%s\" to %s:%d every %dms (port %d)\n",
           self.name, inet_ntoa(to->sin_addr), ntohs(to->sin_port),
           every, port);

    while (running) {
        struct pollfd pfd;
        uint32_t t = now_ms();

        if ((int32_t)(t - next) >= 0) {
            n = nc_beacon_build(out, sizeof(out), &self);
            if (n > 0 &&
                sendto(fd, out, (size_t)n, 0, (const struct sockaddr *)to,
                       sizeof(*to)) < 0 && errno != ENETUNREACH)
                fprintf(stderr, "announce: %s\n", strerror(errno));
            next = t + (uint32_t)every;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) {
            uint8_t rx[NC_BEACON_MAX];
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t got;

            got = recvfrom(fd, rx, sizeof(rx), 0,
                           (struct sockaddr *)&from, &flen);
            if (got > 0 && nc_beacon_kind(rx, (size_t)got) == NC_BEACON_PROBE)
                answer_probe(fd, rx, (size_t)got, &from, &lim, &self);
        }
    }
    close(fd);
    printf("\nstopped\n");
    return 0;
}

/****************************************************************
 * Browsing
 ****************************************************************/

struct seen {
    uint32_t instance;
    uint32_t addr;
    uint32_t last;
    uint16_t players;
    int      used;
};

static void
show(const struct nc_beacon *b, const struct sockaddr_in *from, const char *why)
{
    printf("%-8s %-24s %s:%u  %u/%u  v%u.%u  %s%s%s\n",
           why, b->name[0] ? b->name : "(unnamed)",
           inet_ntoa(from->sin_addr), b->port,
           b->players, b->capacity, b->major, b->minor,
           (b->flags & NC_BEACON_F_CREDENTIALS) ? "login" : "open",
           (b->flags & NC_BEACON_F_REGISTER) ? ", register" : "",
           b->has_key ? ", key" : "");
}

static int
browse(int port, const struct sockaddr_in *to, int seconds)
{
    struct seen list[MAX_SEEN];
    uint8_t probe[NC_BEACON_MAX];
    uint32_t deadline, next_probe = 0;
    int fd, found = 0;
    long plen;

    memset(list, 0, sizeof(list));
    fd = bind_udp(port, 1);
    if (fd < 0) {
        fprintf(stderr, "cannot bind port %d: %s\n", port, strerror(errno));
        return 1;
    }
    plen = nc_beacon_probe(probe, sizeof(probe), GAME_ID);
    if (plen < 0) {
        close(fd);
        return 1;
    }

    printf("looking for servers on %s:%d, listening on %d\n",
           inet_ntoa(to->sin_addr), ntohs(to->sin_port), port);
    deadline = now_ms() + (uint32_t)seconds * 1000;

    while (running && (seconds <= 0 || (int32_t)(now_ms() - deadline) < 0)) {
        struct pollfd pfd;
        uint32_t t = now_ms();
        int i;

        /* Probing repeatedly costs one small packet and covers a server that
         * started after we did. The announcements alone would find it too,
         * just an interval later. */
        if ((int32_t)(t - next_probe) >= 0) {
            if (sendto(fd, probe, (size_t)plen, 0,
                       (const struct sockaddr *)to, sizeof(*to)) < 0)
                fprintf(stderr, "probe: %s\n", strerror(errno));
            next_probe = t + 2000;
        }

        for (i = 0; i < MAX_SEEN; i++)
            if (list[i].used && t - list[i].last > FORGET_MS)
                list[i].used = 0;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN)) {
            uint8_t rx[NC_BEACON_MAX];
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            struct nc_beacon b;
            ssize_t got;
            int slot = -1, fresh = 1;

            got = recvfrom(fd, rx, sizeof(rx), 0,
                           (struct sockaddr *)&from, &flen);
            if (got <= 0)
                continue;
            /* Our own probe comes back when we broadcast. Everything that is
             * not an announcement for our game is simply not interesting. */
            if (nc_beacon_parse(&b, rx, (size_t)got) != NC_BEACON_OK ||
                b.game != GAME_ID)
                continue;

            for (i = 0; i < MAX_SEEN; i++) {
                if (list[i].used && list[i].instance == b.instance &&
                    list[i].addr == from.sin_addr.s_addr) {
                    slot = i;
                    fresh = 0;
                    break;
                }
                if (!list[i].used && slot < 0)
                    slot = i;
            }
            if (slot < 0)
                continue;               /* list full; ignore the rest */

            if (fresh) {
                show(&b, &from, "found");
                found = 1;
            } else if (list[slot].players != b.players) {
                show(&b, &from, "update");
            }
            list[slot].used = 1;
            list[slot].instance = b.instance;
            list[slot].addr = from.sin_addr.s_addr;
            list[slot].players = b.players;
            list[slot].last = t;
        }
    }
    close(fd);

    /* The commonest way to see nothing is to run both halves on one machine,
     * where the broadcast never comes back. Saying so beats leaving someone to
     * wonder whether their network or this program is broken. */
    if (!found && to->sin_addr.s_addr == htonl(INADDR_BROADCAST))
        printf("nothing found. A server on this same machine will not be seen:"
               " a broadcast is not delivered back to the host that sent it."
               " Try --to <that host's address>.\n");
    return 0;
}

/****************************************************************
 * Arguments
 ****************************************************************/

static void
usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  beacon_demo serve  [--port N] [--name S] [--to ADDR[:PORT]]"
            " [--every MS]\n"
            "  beacon_demo browse [--port N] [--to ADDR[:PORT]]"
            " [--seconds N]\n");
}

int
main(int argc, char **argv)
{
    struct sockaddr_in to;
    const char *name = "demo server";
    const char *dest = NULL;
    int port = DEFAULT_PORT, every = DEFAULT_EVERY, seconds = 0;
    int i, serving;

    if (argc < 2) {
        usage();
        return 2;
    }
    if (strcmp(argv[1], "serve") == 0) {
        serving = 1;
    } else if (strcmp(argv[1], "browse") == 0) {
        serving = 0;
    } else {
        usage();
        return 2;
    }

    for (i = 2; i < argc; i++) {
        const char *arg = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;

        if (strcmp(arg, "--port") == 0 && val) {
            port = atoi(val);
            i++;
        } else if (strcmp(arg, "--name") == 0 && val) {
            name = val;
            i++;
        } else if (strcmp(arg, "--to") == 0 && val) {
            dest = val;
            i++;
        } else if (strcmp(arg, "--every") == 0 && val) {
            every = atoi(val);
            i++;
        } else if (strcmp(arg, "--seconds") == 0 && val) {
            seconds = atoi(val);
            i++;
        } else {
            usage();
            return 2;
        }
    }

    if (parse_to(dest ? dest : "255.255.255.255", DEFAULT_PORT, &to) != 0) {
        fprintf(stderr, "bad --to address\n");
        return 2;
    }
    if (every < 100)
        every = 100;

    signal(SIGINT, on_sigint);
    setvbuf(stdout, NULL, _IOLBF, 0);

    return serving ? serve(port, name, &to, every) : browse(port, &to, seconds);
}
