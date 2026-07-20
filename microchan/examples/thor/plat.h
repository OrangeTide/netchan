/* plat.h : platform layer (text screen, keyboard, timing) for the game */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>

#define SCR_W 40
#define SCR_H 25

/* logical keys polled each frame */
enum {
    K_UP, K_DOWN, K_LEFT, K_RIGHT,      /* movement (arrows) */
    K_FIRE, K_QUIT, K_ENTER,
    K_FIRE_UP, K_FIRE_DOWN,             /* directional fire (WASD) */
    K_FIRE_LEFT, K_FIRE_RIGHT,
    K_NKEYS,
};

/* CGA text attribute helpers: attr = (bg << 4) | fg */
#define ATTR(fg, bg) (uint8_t)(((bg) << 4) | (fg))
enum {
    C_BLACK, C_BLUE, C_GREEN, C_CYAN, C_RED, C_MAGENTA, C_BROWN, C_GREY,
    C_DGREY, C_LBLUE, C_LGREEN, C_LCYAN, C_LRED, C_LMAGENTA, C_YELLOW, C_WHITE,
};

void plat_init(void);
void plat_shutdown(void);

uint32_t plat_now_ms(void);

/** Refresh input state (and pump any typed characters). */
void plat_poll(void);

/** Nonzero if the logical key is currently down. */
int plat_key(int k);

/** Next typed ASCII character for chat entry, or 0 if none. */
int plat_getch(void);

/** Write one screen cell. */
void plat_put(int x, int y, unsigned char ch, unsigned char attr);

/** Flush the frame to the display (no-op where writes are direct). */
void plat_present(void);

/** Write the current screen as a 40x25 ASCII grid to a file (for capture
 *  and headless verification). */
void plat_dump(const char *path);

#endif /* PLAT_H */
