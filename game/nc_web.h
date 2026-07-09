/* nc_web.h : browser WebSocket transport for netchan (wasm side) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_WEB_H
#define NC_WEB_H

#include <stddef.h>
#include <stdint.h>
#include "nc_addr.h"

/*
 * The browser end of the WebSocket transport. It is the mirror of nc_udp:
 * where nc_udp is the only file that knows about sockaddr, this is the only
 * file that knows about the browser's WebSocket object. Everything above it,
 * netchan and the game, is identical to the native build.
 *
 * There is no framing here. The browser's WebSocket already delivers whole
 * binary messages, so one netchan datagram maps to one message and the
 * datagram boundary survives on its own. Sending is a call into JavaScript;
 * receiving is JavaScript calling back in.
 */

/* The single peer reachable over a browser WebSocket is the gateway. netchan
 * copies this handle but never reads it, so any non-empty value works. */
extern const struct nc_addr NC_WEB_PEER;

/* Hand one netchan datagram to the browser to send as a binary message.
 * Implemented in terms of Module.wsSend (see nc_web.c). */
void nc_web_send(const void *data, size_t len);

/* Staging buffer for inbound datagrams. JavaScript copies a received binary
 * message into this buffer and then calls the app's receive entry point with
 * the length, which feeds it to netchan against NC_WEB_PEER. */
#define NC_WEB_INBUF 2048
uint8_t *nc_web_inbuf(void);

#endif /* NC_WEB_H */
