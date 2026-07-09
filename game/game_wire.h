/* game_wire.h : pack/unpack the dynamic world state for a Snapshot */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef GAME_WIRE_H
#define GAME_WIRE_H

#include "game.h"
#include <stddef.h>

/*
 * The static map is reproduced on the client from the join seed, so a
 * snapshot only carries the dynamic entities. Everything is packed
 * byte-by-byte in a fixed order and size, which keeps it portable across
 * the native server and the little-endian wasm client without depending
 * on struct layout.
 *
 *   players   : x, y, facing, alive, hp, score_lo, score_hi   (7 each)
 *   creatures : x, y, kind, alive                             (4 each)
 *   shots     : x, y, dir, alive, owner, ttl                  (6 each)
 */
#define GW_STATE_SIZE (MAX_PLAYERS * 7 + MAX_CREATURES * 4 + MAX_SHOTS * 6)

/* Pack the dynamic state of w into buf (>= GW_STATE_SIZE). Returns the
 * number of bytes written, or 0 if buf is too small. */
size_t gw_pack(const struct world *w, uint8_t *buf, size_t cap);

/* Unpack a snapshot into w's player/creature/shot arrays. The map, tick,
 * and nplayers are left untouched. Returns 0 on success, -1 on a short
 * buffer. */
int gw_unpack(struct world *w, const uint8_t *buf, size_t len);

#endif /* GAME_WIRE_H */
