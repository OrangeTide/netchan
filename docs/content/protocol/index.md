---
title: Protocol overview
weight: 3
abstract: Datagrams, frames, channels, and how a session is established.
category: manual
---

A session begins with a two-message handshake. The client sends a `CONNECT_INIT` carrying the connection id it chose; the server replies with `CONNECT_ACCEPT` carrying its own id and the idle timeout it will enforce. Both ends are then `CONNECTED` and may open channels.

A channel opens with a `CHANNEL_OPEN` frame that names its type and its content type, a short string that labels what the channel carries. The peer answers with a `WINDOW_UPDATE` that grants send credit. Data then flows as `DATA` frames, acknowledged by `ACK` on a reliable channel and left unacknowledged on an unreliable one. When a side leaves, a `DISCONNECT` frame ends the session at once rather than leaving the peer to notice the silence when the idle timer expires.

![handshake](handshake.svg)

Several frames travel in one datagram. A datagram is a small header followed by a run of frames, so a single packet can carry a channel's data, an acknowledgement for another channel, and a keepalive at once. The [protocol reference](../protocol-reference/) lists the header and every
frame type.

