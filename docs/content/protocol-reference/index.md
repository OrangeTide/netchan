---
title: Protocol reference
weight: 9
abstract: Frame layouts, header bits, and the constants that bound the implementation.
category: manual
---

Every datagram starts with a header. The first byte is a flag set. A client's opening packet, before it knows the server's id, uses the short init header; every other packet uses the full header with a packet number.

  Header   Bytes   Layout
  -------- ------- ----------------------------------------------
  init     5       flags(1), connection id(4)
  full     7       flags(1), connection id(4), packet number(2)

The header is followed by one or more frames. Each frame starts with a one-byte type.

  Frame                Type   Carries
  -------------------- ------ -------------------------------
  `PADDING`            0x00   nothing; pads a datagram
  `CONNECT_INIT`       0x01   client id, opens a session
  `CONNECT_ACCEPT`     0x02   server id and idle timeout
  `CONNECT_REDIRECT`   0x03   an address to retry against
  `DISCONNECT`         0x04   ends the session
  `PING`               0x05   keepalive probe
  `PONG`               0x06   keepalive reply
  `CHANNEL_OPEN`       0x07   channel type and content type
  `CHANNEL_CLOSE`      0x08   closes a channel
  `DATA`               0x09   channel payload
  `ACK`                0x0A   acknowledges reliable data
  `WINDOW_UPDATE`      0x0B   grants send credit

A reliable channel retransmits an unacknowledged message with an exponential backoff, from the retransmit interval up to a ceiling, and gives up after a fixed number of attempts, at which point the channel is reported closed. Flow control is a credit window: a receiver grants bytes with `WINDOW_UPDATE` and a sender may not exceed the outstanding grant. A message larger than the path can hold is split into fragments and reassembled at the far end.

A connection sends a `PING` after a few seconds of silence and treats the session as dead if nothing arrives before the idle timeout. The defaults below come from `netchan_cfg_default()` and can be changed per connection with `netchan_config()`.

  Setting          Default       Meaning
  ---------------- ------------- -------------------------------------------
  MTU              1200 bytes    largest datagram netchan will build
  channel window   65536 bytes   initial receive credit per channel
  idle timeout     30000 ms      silence before a session is declared dead
  retransmit       100 ms        base interval before the first resend
  keepalive        5000 ms       silence before a PING is sent
  channels         16            concurrent channels per connection
  message          2048 bytes    largest message a channel will buffer

