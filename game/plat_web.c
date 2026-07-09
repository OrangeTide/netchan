/* plat_web.c : browser platform layer (canvas text grid, keyboard, timing) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Implements plat.h for the browser so render.c works unchanged: writes
 * land in a 40x25 cell grid (char + CGA attribute per cell) and are blitted
 * to a <canvas> by JS. Keyboard state and the clock come from JS too.
 */

#include "plat.h"
#include <emscripten.h>
#include <string.h>

static uint8_t  cells[SCR_W * SCR_H * 2];   /* interleaved char, attr */
static unsigned keymask;

/* JS draws the grid; Module.drawGrid reads it out of the wasm heap. */
EM_JS(void, web_blit, (const uint8_t *grid, int w, int h), {
    if (Module.drawGrid)
        Module.drawGrid(HEAPU8.subarray(grid, grid + w * h * 2), w, h);
});
EM_JS(unsigned, web_keys, (void), { return (Module.keymask | 0); });
EM_JS(double,   web_now,  (void), { return performance.now(); });

void
plat_init(void)
{
    memset(cells, 0, sizeof(cells));
}

void plat_shutdown(void) {}

uint32_t
plat_now_ms(void)
{
    return (uint32_t)web_now();
}

void
plat_poll(void)
{
    keymask = web_keys();
}

int
plat_key(int k)
{
    return (int)((keymask >> k) & 1u);
}

int plat_getch(void) { return 0; }

void
plat_put(int x, int y, unsigned char ch, unsigned char attr)
{
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) {
        size_t i = ((size_t)y * SCR_W + (size_t)x) * 2;
        cells[i] = ch;
        cells[i + 1] = attr;
    }
}

void
plat_present(void)
{
    web_blit(cells, SCR_W, SCR_H);
}

void plat_dump(const char *path) { (void)path; }
