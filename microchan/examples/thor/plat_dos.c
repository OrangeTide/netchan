/* plat_dos.c : 16-bit DOS implementation of the platform layer */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "plat.h"
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <stdio.h>

/* keyboard scancodes (set 1) */
#define SC_ESC   0x01
#define SC_Q     0x10
#define SC_W     0x11
#define SC_A     0x1E
#define SC_S     0x1F
#define SC_D     0x20
#define SC_Z     0x2C
#define SC_ENTER 0x1C
#define SC_SPACE 0x39
#define SC_UP    0x48
#define SC_DOWN  0x50
#define SC_LEFT  0x4B
#define SC_RIGHT 0x4D

/* Video page flipping. Mode 01h (40x25) gives 8 display pages in the 16 KB
 * CGA text buffer; each page is 2 KB. We render into the hidden page and, at
 * vertical retrace, point the CRTC at it. The displayed page is never touched
 * while it is on screen, so there is no tearing or mid-rewrite flicker. */
#define VIDEO_SEG  0xB800
#define PAGE_BYTES 0x800            /* 2 KB page granularity for 40x25 */
static int draw_page = 1;           /* hidden page we render into */
static int show_page = 0;           /* page currently displayed */

static volatile unsigned char keystate[128];
static volatile unsigned char kbring[16];   /* typed ASCII, for chat entry */
static volatile unsigned kb_head, kb_tail;
static void (__interrupt __far *old_int09)(void);
static int installed;

/* US-layout scancode -> ASCII; returns 0 for keys with no text meaning */
static unsigned char
sc_to_ascii(unsigned char sc, int shift)
{
    switch (sc) {
    case 0x02: return shift ? '!' : '1';
    case 0x03: return shift ? '@' : '2';
    case 0x04: return shift ? '#' : '3';
    case 0x05: return shift ? '$' : '4';
    case 0x06: return shift ? '%' : '5';
    case 0x07: return shift ? '^' : '6';
    case 0x08: return shift ? '&' : '7';
    case 0x09: return shift ? '*' : '8';
    case 0x0A: return shift ? '(' : '9';
    case 0x0B: return shift ? ')' : '0';
    case 0x0C: return shift ? '_' : '-';
    case 0x0D: return shift ? '+' : '=';
    case 0x0E: return 8;                 /* backspace */
    case 0x1C: return '\r';              /* enter */
    case 0x01: return 27;                /* escape */
    case 0x39: return ' ';
    case 0x10: return shift ? 'Q' : 'q';
    case 0x11: return shift ? 'W' : 'w';
    case 0x12: return shift ? 'E' : 'e';
    case 0x13: return shift ? 'R' : 'r';
    case 0x14: return shift ? 'T' : 't';
    case 0x15: return shift ? 'Y' : 'y';
    case 0x16: return shift ? 'U' : 'u';
    case 0x17: return shift ? 'I' : 'i';
    case 0x18: return shift ? 'O' : 'o';
    case 0x19: return shift ? 'P' : 'p';
    case 0x1A: return shift ? '{' : '[';
    case 0x1B: return shift ? '}' : ']';
    case 0x1E: return shift ? 'A' : 'a';
    case 0x1F: return shift ? 'S' : 's';
    case 0x20: return shift ? 'D' : 'd';
    case 0x21: return shift ? 'F' : 'f';
    case 0x22: return shift ? 'G' : 'g';
    case 0x23: return shift ? 'H' : 'h';
    case 0x24: return shift ? 'J' : 'j';
    case 0x25: return shift ? 'K' : 'k';
    case 0x26: return shift ? 'L' : 'l';
    case 0x27: return shift ? ':' : ';';
    case 0x28: return shift ? '"' : '\'';
    case 0x29: return shift ? '~' : '`';
    case 0x2B: return shift ? '|' : '\\';
    case 0x2C: return shift ? 'Z' : 'z';
    case 0x2D: return shift ? 'X' : 'x';
    case 0x2E: return shift ? 'C' : 'c';
    case 0x2F: return shift ? 'V' : 'v';
    case 0x30: return shift ? 'B' : 'b';
    case 0x31: return shift ? 'N' : 'n';
    case 0x32: return shift ? 'M' : 'm';
    case 0x33: return shift ? '<' : ',';
    case 0x34: return shift ? '>' : '.';
    case 0x35: return shift ? '?' : '/';
    default:   return 0;
    }
}

static void __interrupt __far
kb_isr(void)
{
    unsigned char sc = (unsigned char)inp(0x60);
    if (sc != 0xE0) {           /* ignore the extended-key prefix byte */
        unsigned char code = sc & 0x7F;
        int make = (sc & 0x80) ? 0 : 1;
        keystate[code] = (unsigned char)make;
        if (make) {
            unsigned char c = sc_to_ascii(code,
                                  keystate[0x2A] || keystate[0x36]);
            if (c) {
                unsigned nx = (kb_head + 1) & 15;
                if (nx != kb_tail) {
                    kbring[kb_head] = c;
                    kb_head = nx;
                }
            }
        }
    }
    outp(0x20, 0x20);           /* end of interrupt */
}

void
plat_init(void)
{
    union REGS r;
    int i;

    r.h.ah = 0x00;              /* set video mode 01h: 40x25 16-color text */
    r.h.al = 0x01;
    int86(0x10, &r, &r);

    r.h.ah = 0x01;              /* hide the hardware cursor */
    r.w.cx = 0x2000;
    int86(0x10, &r, &r);

    /* clear both pages so the first hidden frame holds no garbage */
    {
        unsigned char __far *v = (unsigned char __far *)MK_FP(VIDEO_SEG, 0);
        unsigned n;
        for (n = 0; n < 2 * PAGE_BYTES; n += 2) {
            v[n] = ' ';
            v[n + 1] = ATTR(C_GREY, C_BLACK);
        }
    }
    draw_page = 1;
    show_page = 0;

    for (i = 0; i < 128; i++)
        keystate[i] = 0;
    old_int09 = _dos_getvect(0x09);
    _dos_setvect(0x09, kb_isr);
    installed = 1;
}

void
plat_shutdown(void)
{
    union REGS r;
    if (installed) {
        _dos_setvect(0x09, old_int09);
        installed = 0;
    }
    r.h.ah = 0x00;             /* back to 80x25 text */
    r.h.al = 0x03;
    int86(0x10, &r, &r);
}

uint32_t
plat_now_ms(void)
{
    volatile uint32_t __far *t = (volatile uint32_t __far *)
        MK_FP(0x0040, 0x006C);
    return (*t) * 55u;
}

void
plat_poll(void)
{
    /* Keyboard state is maintained by the interrupt handler. Make sure
     * interrupts are enabled every frame: if an IPX far call or our own
     * ISR ever returns with them disabled, the BIOS timer tick (which our
     * timing and frame pacing read) would freeze. */
    _enable();
}

int
plat_key(int k)
{
    switch (k) {
    case K_UP:    return keystate[SC_UP];
    case K_DOWN:  return keystate[SC_DOWN];
    case K_LEFT:  return keystate[SC_LEFT];
    case K_RIGHT: return keystate[SC_RIGHT];
    case K_FIRE:  return keystate[SC_Z] || keystate[SC_SPACE];
    case K_QUIT:  return keystate[SC_Q] || keystate[SC_ESC];
    case K_ENTER: return keystate[SC_ENTER];
    case K_FIRE_UP:    return keystate[SC_W];
    case K_FIRE_DOWN:  return keystate[SC_S];
    case K_FIRE_LEFT:  return keystate[SC_A];
    case K_FIRE_RIGHT: return keystate[SC_D];
    default:      return 0;
    }
}

int
plat_getch(void)
{
    unsigned char c;
    if (kb_head == kb_tail)
        return 0;
    c = kbring[kb_tail];
    kb_tail = (kb_tail + 1) & 15;
    return c;
}

void
plat_put(int x, int y, unsigned char ch, unsigned char attr)
{
    unsigned char __far *v;
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H)
        return;
    v = (unsigned char __far *)MK_FP(VIDEO_SEG,
        (unsigned)(draw_page * PAGE_BYTES) + (unsigned)(y * SCR_W + x) * 2);
    v[0] = ch;
    v[1] = attr;
}

/* Wait for the start of vertical retrace (0x3DA bit 3). Polling out of an
 * in-progress retrace first avoids latching onto the tail of the current one. */
static void
wait_vretrace(void)
{
    while (inp(0x3DA) & 0x08)
        ;
    while (!(inp(0x3DA) & 0x08))
        ;
}

void
plat_present(void)
{
    union REGS r;

    wait_vretrace();
    r.h.ah = 0x05;              /* select active display page */
    r.h.al = (unsigned char)draw_page;
    int86(0x10, &r, &r);

    show_page = draw_page;
    draw_page ^= 1;            /* next frame renders into the other page */
}

static unsigned char
ascii_of(unsigned char ch)
{
    switch (ch) {
    case 0x01: return 'o';
    case 0x07: return '*';
    case 0x18: return '^';      /* shot heading north */
    case 0x19: return 'v';      /* shot heading south */
    case 0x1A: return '>';      /* shot heading east  */
    case 0x1B: return '<';      /* shot heading west  */
    case 0xB1: return '#';
    case 0xDB: return '#';
    case 0xFA: return '.';
    }
    return (ch >= 32 && ch < 127) ? ch : ' ';
}

void
plat_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    int x, y;
    if (!f)
        return;
    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            unsigned char __far *v = (unsigned char __far *)
                MK_FP(VIDEO_SEG, (unsigned)(show_page * PAGE_BYTES) +
                      (unsigned)(y * SCR_W + x) * 2);
            putc(ascii_of(v[0]), f);
        }
        putc('\n', f);
    }
    fclose(f);
}
