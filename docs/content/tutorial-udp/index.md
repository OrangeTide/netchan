---
title: Tutorial: UDP peer to peer
weight: 4
abstract: A working peer in one file: open a socket, feed netchan, drain what it wants to send.
category: manual
---

The application owns the socket. netchan tells it what to send and when to ask again; moving the bytes is the application's job. The shape of every program is the same loop: drain what netchan wants to send, wait for the socket, feed what arrives, then drain the events.

    struct netchan_conn *c = netchan_open(0);   /* 0 = client, 1 = server */
    netchan_connect(c, &server_addr);

    struct netchan_chan *ch = netchan_chan_open(c, NETCHAN_RELIABLE,
                                                NETCHAN_DIR_SEND, "state");
    netchan_chan_write(ch, msg, len);

    for (;;) {
        uint8_t pkt[1500];
        struct nc_addr to;
        size_t n;
        while ((n = netchan_send_next(c, pkt, sizeof pkt, &to)) > 0)
            sendto_however_you_like(pkt, n, &to);

        int wait_ms = netchan_service(c, now_ms());
        /* wait for the socket to be readable, up to wait_ms */

        if (got_a_datagram)
            netchan_feed(c, rx, rxlen, &from);

        struct netchan_event ev;
        while (netchan_poll(c, &ev))
            if (ev.type == NETCHAN_EV_DATA)
                netchan_chan_read(ev.ch, buf, sizeof buf);
    }

`netchan_service()` returns the milliseconds until it next needs to run, so a poll loop can sleep exactly that long. It fires retransmit timers, sends a keepalive on an idle connection, and raises the idle-timeout disconnect when a peer goes silent. Calling it too often is harmless; not calling it at all stalls retransmission.

To leave cleanly, disconnect, flush the frame that queues, then close. `netchan_close()` frees the connection and tells the peer nothing, and netchan owns no socket, so the `DISCONNECT` that `netchan_disconnect()` queues goes nowhere until you send it yourself.

    netchan_disconnect(c);
    while ((n = netchan_send_next(c, pkt, sizeof pkt, &to)) > 0)
        sendto_however_you_like(pkt, n, &to);
    netchan_close(c);

