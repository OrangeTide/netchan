/* nc_web.c : browser WebSocket transport for netchan (wasm side) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_web.h"

#include <emscripten.h>

const struct nc_addr NC_WEB_PEER = { 1, { 1 } };

static uint8_t inbuf[NC_WEB_INBUF];

uint8_t *
nc_web_inbuf(void)
{
    return inbuf;
}

/* Send a datagram by handing the bytes to the page's WebSocket. The view is
 * copied on the JS side because ws.send may keep the buffer past this call. */
EM_JS(void, nc_web_send, (const void *data, size_t len), {
    if (Module.wsSend)
        Module.wsSend(HEAPU8.slice(data, data + len));
});
