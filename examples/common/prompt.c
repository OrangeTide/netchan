/* prompt.c : read a line from the terminal without echoing it */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "prompt.h"

#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

void
prompt_reader_begin(struct prompt_reader *pr, const char *prompt)
{
    struct termios quiet;

    memset(pr, 0, sizeof(*pr));
    pr->active = 1;
    pr->is_tty = isatty(STDIN_FILENO);

    if (prompt) {
        fputs(prompt, stderr);
        fflush(stderr);
    }

    /* Clearing ECHO is the whole reason this file exists. A line editor
     * would not help: linenoise and readline both echo by design, so a
     * password prompt is exactly the case they do not cover. */
    if (pr->is_tty && tcgetattr(STDIN_FILENO, &pr->old) == 0) {
        quiet = pr->old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0)
            pr->restored = 1;
    }
}

int
prompt_reader_feed(struct prompt_reader *pr, int fd)
{
    char ch;
    ssize_t r;

    if (!pr->active)
        return -1;

    /*
     * One byte per call, straight from the descriptor. Reading in bulk or
     * through stdio would consume the lines *after* this one into a buffer
     * nobody looks in again, which on a pipe silently eats the rest of the
     * input. The caller is woken again while bytes remain.
     */
    r = read(fd, &ch, 1);
    if (r <= 0) {
        pr->buf[pr->len] = '\0';
        return pr->len > 0 ? 1 : -1;    /* a last line without a newline */
    }

    if (ch == '\n') {
        pr->buf[pr->len] = '\0';
        return 1;
    }
    if (ch != '\r' && pr->len + 1 < sizeof(pr->buf))
        pr->buf[pr->len++] = ch;
    return 0;
}

void
prompt_reader_end(struct prompt_reader *pr)
{
    if (!pr->active)
        return;
    if (pr->restored)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &pr->old);
    if (pr->is_tty)
        fputc('\n', stderr);
    memset(pr->buf, 0, sizeof(pr->buf));
    pr->len = 0;
    pr->active = 0;
    pr->restored = 0;
}

/*
 * The blocking form, for the tools that have no event loop to keep turning.
 * It is the incremental reader spun in place rather than a second copy of
 * the terminal handling.
 */
int
prompt_hidden(const char *prompt, char *buf, size_t cap)
{
    struct prompt_reader pr;
    int r;

    prompt_reader_begin(&pr, prompt);
    while ((r = prompt_reader_feed(&pr, STDIN_FILENO)) == 0)
        ;
    if (r > 0)
        snprintf(buf, cap, "%s", pr.buf);
    else
        buf[0] = '\0';
    prompt_reader_end(&pr);
    return r > 0 ? 0 : -1;
}
