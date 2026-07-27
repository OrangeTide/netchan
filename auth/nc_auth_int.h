/* nc_auth_int.h : what the two halves of the login conversation share */

#ifndef NC_AUTH_INT_H
#define NC_AUTH_INT_H

#include "nc_auth.h"

/*
 * nc_auth.c holds the ssh-shaped part, publickey and password, which has not
 * changed since it was written. nc_auth_ia.c holds the interactive method,
 * which is most of the code and none of the cryptography. They meet here.
 */

/* Message types. */
#define MSG_HELLO      0x10     /* C->S [ulen:1][user]          */
#define MSG_METHODS    0x11     /* S->C [mask:1]                */
#define MSG_PUBKEY     0x12     /* C->S [pk:32][sig:64]         */
#define MSG_PASSWORD   0x13     /* C->S [plen:1][password]      */
#define MSG_OK         0x14     /* S->C []                      */
#define MSG_FAIL       0x15     /* S->C [methods_left:1]        */
#define MSG_IA_BEGIN   0x16     /* C->S [method:1][token]       */
#define MSG_IA_FORM    0x17     /* S->C [title][note][fields]   */
#define MSG_IA_SUBMIT  0x18     /* C->S [answers]               */
#define MSG_IA_WAIT    0x19     /* S->C [note][token]           */
#define MSG_IA_CANCEL  0x1a     /* C->S []                      */

/* True for the messages whose size is bounded by the caller's buffer rather
 * than by NC_AUTH_MAX_MSG. */
#define MSG_IS_BULK(t)  ((t) == MSG_IA_FORM || (t) == MSG_IA_SUBMIT)

/*
 * microser field numbers. Numbering is per message, but the wait message
 * carries both a note and a token, so no two of these may collide even where
 * the messages that use them never meet.
 */
#define F_TITLE   1
#define F_NOTE    2
#define F_FIELDS  3
#define F_TOKEN   4
#define F_METHOD  1
#define F_ANSWERS 1

void nc_auth_i_emit(struct nc_auth *a, const uint8_t *msg, size_t len);
void nc_auth_i_client_try_next(struct nc_auth *a);
void nc_auth_i_server_deny(struct nc_auth *a, unsigned spent);
void nc_auth_i_server_offer(struct nc_auth *a);

int  nc_auth_i_ia_client(struct nc_auth *a, const uint8_t *msg, size_t len);
int  nc_auth_i_ia_server(struct nc_auth *a, const uint8_t *msg, size_t len);
void nc_auth_i_ia_choose(struct nc_auth *a);
void nc_auth_i_ia_end(struct nc_auth *a);

#endif /* NC_AUTH_INT_H */
