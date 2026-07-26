/* prompt.h : read a line from the terminal without echoing it */

#ifndef PROMPT_H
#define PROMPT_H

#include <stddef.h>
#include <termios.h>

/*
 * No PROMPT_OK: prompt_reader_feed is tri-state, returning 1 for a complete
 * line, 0 when more input is needed, and PROMPT_ERR on EOF or error.
 */
#define PROMPT_ERR (-1)

#define PROMPT_MAX 256

/*
 * Write prompt to the terminal, read one line into buf with echo disabled,
 * and strip the newline. Returns 0 on success, -1 on EOF or error.
 *
 * This one blocks, which is correct for a tool that has nothing else to do
 * while it waits. Programs driven by an event loop want prompt_reader below.
 */
int prompt_hidden(const char *prompt, char *buf, size_t cap);

/*
 * The same thing spread across event-loop wakeups, for a program that cannot
 * afford to stop. Begin the prompt, feed it whenever the descriptor is
 * readable, and collect the line when a feed reports it is complete.
 *
 * Bytes are taken one at a time. Reading in bulk would be faster and wrong:
 * on a pipe there is no line discipline, so a bulk read happily consumes the
 * lines *after* this one into a buffer nobody will look in again.
 */
struct prompt_reader {
    char           buf[PROMPT_MAX];
    size_t         len;
    struct termios old;
    int            restored;    /* the terminal mode needs putting back */
    int            is_tty;
    int            active;
};

/* Draw the prompt and turn echo off. */
void prompt_reader_begin(struct prompt_reader *pr, const char *prompt);

/* Consume one byte. Returns 1 when the line is complete and NUL terminated
 * in pr->buf, 0 when more is needed, PROMPT_ERR on EOF or error. */
int prompt_reader_feed(struct prompt_reader *pr, int fd);

/* Restore the terminal and wipe the buffer. Safe to call more than once. */
void prompt_reader_end(struct prompt_reader *pr);

#endif /* PROMPT_H */
