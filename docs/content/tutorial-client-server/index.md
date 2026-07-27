---
title: Tutorial: client and server
weight: 5
abstract: One connection object per peer, demultiplexed by connection id.
category: manual
---

The two ends differ only at open and accept. A server opens with `netchan_open(1)` and, on the first datagram from a new peer, calls `netchan_accept()` to complete the handshake. A server that talks to many clients keeps one `struct netchan_conn` per peer, routing an incoming datagram to the right one; `netchan_peek_id()` reads the connection id out of a raw packet without parsing it, which is how a dispatcher picks the connection before feeding the bytes in.

A channel carries a content type set when it opens, and the peer reads it back with `netchan_chan_content_type()`. Use it to tell channels apart when one connection carries several. A server that offers a "chat" channel and a "presence" channel decides which handler a datagram belongs to from that string, before it looks at the payload.

    case NETCHAN_EV_CHAN_OPEN:
        if (strcmp(netchan_chan_content_type(ev.ch), "chat") == 0)
            peer->chat = ev.ch;
        break;

One caution the examples learned the hard way: do not free a connection from inside its own poll loop. Handling `NETCHAN_EV_DISCONNECTED` by calling `netchan_close()` and then looping back to `netchan_poll()` reads a freed pointer. Note that the slot is gone and break out of the loop first.

