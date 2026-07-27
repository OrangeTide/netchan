---
title: Registration and interactive authentication
weight: 12
abstract: How a client with no account asks a public server for one, and the single interactive method that carries registration, passwords, and tokens alike.
category: design
draft: true
---

A public game server meets players it has never seen. The client arrives with no
username, no password, and no enrolled key, so neither of the methods in
[`nc_auth`](../tutorial-auth/) can succeed. Something has to happen before a
login is even possible, and that something is registration: the server describes
a form, the player fills it in, and an account exists at the end of it.

This page describes how that conversation is shaped and where its edges are. It
is a design, not a description of code that exists.

## One method, not a method per scheme

The obvious move is to add a bit next to `NC_AUTH_M_PUBKEY` and
`NC_AUTH_M_PASSWORD` for every new scheme. Registration would get one, one-time
tokens another, a second factor a third. Each bit is protocol surface that has to
be specified, implemented on both ends, and kept compatible forever.

ssh solved this once already. Its keyboard-interactive method carries no scheme
of its own. The server sends prompts, the client renders them, the client sends
the answers back, and the loop repeats until the server is satisfied. Passwords,
one-time pads, PAM modules, and hardware tokens have all ridden that one method
for decades without the protocol learning anything about them.

netchan takes the same approach and adds types to the prompts. A single
interactive method carries registration, password entry, email confirmation,
SMS codes, and whatever an application invents later. The protocol defines the
loop and the field vocabulary. It never defines a scheme.

## Where it sits

Nothing in the protocol core changes. Authentication is a conversation on an
ordinary reliable channel, as the [architecture](../architecture/) page
describes, and registration is more of the same conversation.

Messages travel on a reliable stream channel, each one length prefixed. The
stream handles segmentation and reassembly, so a form larger than the MTU is not
a special case and no message size constant needs to grow to accommodate one. A
peer that has not authenticated yet is still capped, in bytes and in messages, to
bound what a stranger can make the server hold.

## Trusting the server on first contact

`nc_crypto` authenticates the server because the client knows the server's
identity key in advance. A player registering for the first time is exactly the
case where that assumption is weakest, and it is also the moment they are about
to type a password.

Trust on first use is the fallback, not the plan. The plan is that the client
already holds the key before it connects, and there are two shapes that fit:

- The installer bundles a `known_hosts` file listing the official servers.
- The game refreshes that file over HTTPS from a site it already trusts.

Both are the application's to build. netchan supplies the `known_hosts` format
and the `verify_peer` callback that consults it, and stops there.

**Threat model.** The concern here is abuse and griefing, not a funded attacker.
The design is best effort: it should make casual mischief expensive and should
not leak credentials to a passive observer, and it does not attempt more than
that. Two consequences are accepted deliberately rather than defended against.

A registration form tells a stranger whether a username is taken, which is an
account enumeration oracle. Every signup flow has one. The alternative is to
register against an identity the server never echoes back and let the display
name be chosen afterward, which is more machinery than the problem deserves
here. Note that this does undo the property `nc_auth`'s demo store protects when
it offers identical methods for known and unknown users. The two cannot both
hold.

Rate limiting is the real defence, and it belongs outside the protocol. An
unauthenticated peer is held to roughly one request per second per address, and
a server that logs refusals can point an external deny daemon at the log to
block a persistent source at the firewall. That is a better tool than anything
netchan could grow, and it is the same arrangement that has protected sshd for
years.

**Amplification is a transport concern, not this one.** The rule that a response
must not exceed the request that provoked it belongs in `nc_crypto` and the
transport, where an unvalidated address can still be spoofed. By the time an
auth message flows, the X25519 handshake has completed, which means the client
received a server packet and answered it, which proves it holds the address it
claims. Applying a size rule to the interactive exchange would only create a
problem that does not exist, since a small request for a form legitimately draws
a large form in reply and the client cannot pad for a size it has not been told
yet.

## The exchange

Registration is offered in `METHODS` like any other method. A client that wants
an account selects it, and the interactive loop begins.

    client -> HELLO        "I claim to be <user>", or no name at all
    server -> METHODS      "publickey, password, register"
    client -> BEGIN        selects the interactive method
    server -> FORM         a field list and a transaction id
    client -> SUBMIT       values for those fields
    server -> FORM         again, with per-field errors, or asking for more
    client -> SUBMIT
    server -> DONE         the account exists

A submission has three possible answers. `DONE` ends the registration. Another
`FORM` continues it, either because a field was rejected or because the next
step needs different questions. `DENIED` ends it without an account.

The loop is not bounded by a fixed number of rounds. Email confirmation, a
second factor, and a corrected typo are all just another turn.

**Registration returns to the state before it.** `DONE` does not authenticate
anyone. It puts the conversation back at the point where the server offered
methods, and the server runs its `methods` callback again, because the answer
has changed: the user now exists. The client then logs in normally, with the
credential it just enrolled.

This costs a few messages and buys a clean separation. There is exactly one path
into an authenticated session, and it is the same one an established player
takes every day.

**Enrolling a key proves possession.** If registration carries a client public
key, the client also signs the digest from `nc_auth_signed_digest`, the same one
a login signs, over the same session id. Recording a bare public key would only
prove someone pasted it. The signature proves the client holds the secret half.

Because the digest is bound to the session id and not to any long-term key, a
client can enrol a key and immediately authenticate with it inside the same
session. The session key and the identity key are unrelated, so there is nothing
to renegotiate and no reason to disconnect.

## The form

A form is a flat array of typed fields, encoded with the
[IDL](../encoding/) like any other message. There is no markup on the wire.

The field types are inspired by HTML forms, and the resemblance ends at the
vocabulary. A tag syntax would mean writing a parser for quoting, entity
references, and malformed nesting, then exposing it to the first message a
stranger can send, and no browser will ever render this anyway. A flat array
costs nothing to decode and cannot be malformed in interesting ways.

| Type | Carries | Attributes beyond the common ones |
|---|---|---|
| `text` | a line of UTF-8 | `pattern` |
| `password` | a line the client must not echo | `pattern` |
| `email` | an address, `user@host` | |
| `integer` | a whole number | `min`, `max` |
| `phone` | a number the client may format by region | |
| `bool` | a single yes or no | |
| `choice` | one of a list, or several | `options`, `multi` |
| `note` | nothing, it is text for the player to read | |
| `link` | a URL for the client to open externally | |

Every field carries a name, a label, and flags for required and for a default
value. `size` and `maxlength` come along as rendering hints, so a client can lay
out a sensible box without guessing.

Four omissions are deliberate.

**No hidden fields.** They exist in HTML because HTTP is stateless and the
browser has to carry server state through the round trip. netchan has a session
and a transaction id, so continuation state lives in server memory keyed by the
transaction. A hidden field would only invite a server to trust data it handed
to an untrusted client and got back.

**No buttons.** Submit and reset are affordances of a document renderer. A game
client draws its own interface and already knows the form has one submit action,
because it is a form. Reset is a widget behaviour with no protocol meaning.

**No separate radio and checkbox.** In HTML those are distinct elements joined
by a shared name, which needs grouping rules a flat array does not have.
`choice` carries its own options and a `multi` flag, so one record describes the
whole question and a peer cannot send half a group.

**No file or image fields.** Nothing in a registration needs them, and both
would drag transfer machinery into a conversation that has none.

`note` and `link` are the two entries above that go beyond an HTML form's input
types. They earn their place on the browser confirmation case: an OpenID or
similar flow is not a question with an answer, it is an instruction to go
somewhere else and come back, and there is no way to express that with input
fields alone.

**Everything is a hint.** `maxlength`, `min`, `max`, `pattern`, and `required`
exist so the client can catch a typo without a round trip. The server revalidates
every one of them on arrival and applies its own limits regardless of what the
form declared. A client that ignores the hints entirely is rude, not dangerous.

Text is UTF-8 throughout.

## Errors

A rejected submission comes back as a form again, with the fields as before and a
message attached to each one that failed. The client renders the message beside
the field it belongs to, which is the difference between "password too short" and
a player guessing which of six boxes upset the server.

A server need not explain every failure. It attaches messages to as many fields
as it cares to, and a client that receives a form with no messages at all still
knows the submission was rejected. Keeping the combined messages inside roughly
500 bytes is a reasonable target. A form is not a place to write an essay, and a
long explanation belongs in a `note` field where it can be read once rather than
repeated on every retry.

## Pending registrations and bearer tokens

A registration that waits on an emailed code stays open for minutes. The
connection itself survives that, because netchan sends a keepalive when a session
is otherwise idle, and the interactive loop has no timer of its own.

The connection is not what fails. The player opens the mail on their phone, or
the game crashes, or the wifi drops, and a registration that lived only in the
connection is lost with it. So a pending registration lives on the server, keyed
by a token handed to the client, and a fresh connection can present that token
and carry on where it left off. The token expires, which is where a server sets
how long it is willing to wait, and the number of pending registrations from one
address is capped, so a stranger cannot hold a hundred of them open.

A one-time login token is a bearer credential, for the case where a player logs
in through a link in an email or a code sent by SMS. Holding it is being the
user. Its security is exactly the security of the channel that delivered it,
which is worth saying plainly rather than dressing up: an emailed token is as
safe as the player's mailbox. Such tokens are single use and short lived.

## What netchan does not do

netchan defines the messages, the state machine, and the field vocabulary. It
carries a form to a client and answers back to a server, and it has no opinion
about what any of it means.

It does not send mail, speak to an SMS gateway, implement OpenID or any other
identity protocol, store accounts, or decide how a password is hashed at rest.
An application that vendors netchan brings all of that. The server callbacks are
where the two meet: netchan asks whether a name is acceptable and whether a
credential is valid, and the answers come from a store netchan never sees.

This is the same line the rest of the library draws. The core never names a
socket, `nc_crypto` never decides whether to trust a key, and the interactive
method never learns what an account is.
