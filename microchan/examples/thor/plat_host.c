/* plat_host.c : POSIX/ANSI implementation of the platform layer */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>

static struct termios saved_tio;
static unsigned char cell_ch[SCR_H][SCR_W];
static uint8_t cell_at[SCR_H][SCR_W];
static int keydown[K_NKEYS];
static int typed[32], typed_head, typed_tail;

/* CGA color index -> ANSI base (0-7); bright if the CGA index >= 8 */
static const int cga_ansi[16] = { 0, 4, 2, 6, 1, 5, 3, 7,
                                  0, 4, 2, 6, 1, 5, 3, 7 };

void
plat_init(void)
{
    struct termios t;
    tcgetattr(0, &saved_tio);
    t = saved_tio;
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) | O_NONBLOCK);
    fputs("\x1b[2J\x1b[?25l", stdout);
    fflush(stdout);
}

void
plat_shutdown(void)
{
    tcsetattr(0, TCSANOW, &saved_tio);
    fputs("\x1b[0m\x1b[?25h\n", stdout);
    fflush(stdout);
}

uint32_t
plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void
plat_poll(void)
{
    unsigned char b[64];
    int n, i;

    for (i = 0; i < K_NKEYS; i++)
        keydown[i] = 0;

    n = (int)read(0, b, sizeof(b));
    for (i = 0; i < n; i++) {
        int ch = -1;
        if (b[i] == 0x1b && i + 2 < n && b[i + 1] == '[') {
            switch (b[i + 2]) {
            case 'A': keydown[K_UP] = 1; break;
            case 'B': keydown[K_DOWN] = 1; break;
            case 'C': keydown[K_RIGHT] = 1; break;
            case 'D': keydown[K_LEFT] = 1; break;
            default: break;
            }
            i += 2;
            continue;
        } else if (b[i] == 'z' || b[i] == ' ') {
            keydown[K_FIRE] = 1;
        } else if (b[i] == 'q') {
            keydown[K_QUIT] = 1;
        } else if (b[i] == 'w') {
            keydown[K_FIRE_UP] = 1;
        } else if (b[i] == 's') {
            keydown[K_FIRE_DOWN] = 1;
        } else if (b[i] == 'a') {
            keydown[K_FIRE_LEFT] = 1;
        } else if (b[i] == 'd') {
            keydown[K_FIRE_RIGHT] = 1;
        }
        /* also feed printable chars, enter, backspace, esc to the typed
         * ring so chat entry works (in play mode the game drains/ignores it) */
        if (b[i] == '\r' || b[i] == '\n') {
            keydown[K_ENTER] = 1;
            ch = '\r';
        } else if (b[i] == 0x7f || b[i] == 0x08) {
            ch = 8;
        } else if (b[i] == 0x1b) {
            ch = 27;
        } else if (b[i] >= 32 && b[i] < 127) {
            ch = b[i];
        }
        if (ch >= 0) {
            int nt = (typed_head + 1) % 32;
            if (nt != typed_tail) {
                typed[typed_head] = ch;
                typed_head = nt;
            }
        }
    }
}

int
plat_key(int k)
{
    return (k >= 0 && k < K_NKEYS) ? keydown[k] : 0;
}

int
plat_getch(void)
{
    int c;
    if (typed_tail == typed_head)
        return 0;
    c = typed[typed_tail];
    typed_tail = (typed_tail + 1) % 32;
    return c;
}

void
plat_put(int x, int y, unsigned char ch, unsigned char attr)
{
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H)
        return;
    cell_ch[y][x] = ch;
    cell_at[y][x] = attr;
}

/* map a CP437 cell to something a UTF-8 terminal can show */
static unsigned char
ascii_of(unsigned char ch)
{
    switch (ch) {
    case 0x01: return 'o';      /* creature */
    case 0x07: return '*';      /* shot */
    case 0x18: return '^';      /* shot heading north */
    case 0x19: return 'v';      /* shot heading south */
    case 0x1A: return '>';      /* shot heading east  */
    case 0x1B: return '<';      /* shot heading west  */
    case 0xB1: return '#';      /* wall */
    case 0xDB: return '#';
    case 0xFA: return '.';      /* floor */
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
        for (x = 0; x < SCR_W; x++)
            putc(ascii_of(cell_ch[y][x]), f);
        putc('\n', f);
    }
    fclose(f);
}

void
plat_present(void)
{
    int x, y, last = -1;
    fputs("\x1b[H", stdout);
    for (y = 0; y < SCR_H; y++) {
        for (x = 0; x < SCR_W; x++) {
            uint8_t a = cell_at[y][x];
            if (a != last) {
                int fg = a & 0x0f, bg = (a >> 4) & 0x07;
                printf("\x1b[0;%d;%dm",
                       (fg >= 8 ? 90 : 30) + cga_ansi[fg],
                       40 + cga_ansi[bg]);
                last = a;
            }
            putchar(ascii_of(cell_ch[y][x]));
        }
        putchar('\n');
    }
    fputs("\x1b[0m", stdout);
    fflush(stdout);
}
