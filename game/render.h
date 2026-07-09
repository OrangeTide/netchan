/* render.h : text-mode rendering of the game world */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef RENDER_H
#define RENDER_H

#include "game.h"

/** Draw the world from a camera centered on player `self`, plus a HUD.
 *  `status` is an optional one-line message shown on the bottom row. */
void render_world(const struct world *w, int self, const char *status);

#endif /* RENDER_H */
