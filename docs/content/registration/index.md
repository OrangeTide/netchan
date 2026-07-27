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

Nothing implements this yet. It is a design, and the code it describes has not
been written.

## One method for every scheme

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
a special case.

The fixed 256-byte message ceiling `nc_auth` carries today does not survive
that, and a form arrives while the conversation is suspended waiting for a
human, so it has to be copied somewhere that outlives the caller's receive
buffer. `nc_auth` allocates nothing and holds nothing that large, so the buffer
comes from the application, which is also where the real cap on an
unauthenticated peer ends up: a stranger can make the server hold exactly as
much as its operator chose to offer.

A stream hands back bytes rather than messages, so somebody has to write the
length in front of each one and reassemble on the far side. `nc_auth` ships
that, because otherwise every caller writes the same twenty lines and some of
them get the partial read wrong. The helper knows nothing about netchan either:
bytes go in, whole messages come out, which is what keeps a test able to drive
both state machines in one process with no transport at all.

## Trusting the server on first contact

`nc_crypto` authenticates the server because the client knows the server's
identity key in advance. A player registering for the first time is exactly the
case where that assumption is weakest, and it is also the moment they are about
to type a password.

The client should already hold the key before it connects, and there are two
shapes that fit:

- The installer bundles a `known_hosts` file listing the official servers.
- The game refreshes that file over HTTPS from a site it already trusts.

Both are the application's to build. netchan supplies the `known_hosts` format
and the `verify_peer` callback that consults it, and stops there. Trust on first
use is what happens when neither is in place.

### Threat model

The concern here is abuse and griefing, not a funded attacker. The design is
best effort: it should make casual mischief expensive and should not leak
credentials to a passive observer, and it attempts no more than that. Two
consequences are accepted deliberately rather than defended against.

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

Amplification belongs to the transport. The rule that a response
must not exceed the request that provoked it lives in `nc_crypto` and below,
where an unvalidated address can still be spoofed. By the time an
auth message flows, the X25519 handshake has completed, which means the client
received a server packet and answered it, which proves it holds the address it
claims. Applying a size rule to the interactive exchange would only create a
problem that does not exist, since a small request for a form legitimately draws
a large form in reply and the client cannot pad for a size it has not been told
yet.

## The exchange

Registration is offered in `METHODS` like any other method. A client that wants
an account chooses it, and the interactive loop begins.

    client -> HELLO        "I claim to be <user>", or no name at all
    server -> METHODS      "publickey, password, register"
    client -> BEGIN        chooses to register rather than log in
    server -> FORM         a field list
    client -> SUBMIT       values for those fields
    server -> FORM         again, with per-field errors, or asking for more
    client -> SUBMIT
    server -> WAIT         "we sent a code to the address you gave"
    server -> FORM         "enter the code"
    client -> SUBMIT
    server -> DONE         the account exists

A submission has four possible answers. `DONE` ends the registration. Another
`FORM` continues it, either because a field was rejected or because the next
step needs different questions. `DENIED` ends it without an account. `WAIT`
says the server has handed the work to something slow and will speak again
when it hears back.

The loop is not bounded by a fixed number of rounds. Email confirmation, a
second factor, and a corrected typo are all just another turn.

### One form is outstanding at a time

A server may send a form in reply to a submission, or once a wait has resolved,
and at no other time. That is the invariant the rest of the exchange rests on,
and it is worth stating because a future server author will otherwise reach for
an unsolicited re-prompt and quietly break it.

What it buys is that a form and its answers need nothing to tie them together.
The channel is reliable and ordered, the conversation is lock-step, and the only
message a client may send after a form is the answer to that form. There is
never a second candidate, so there is no identifier to carry and no bookkeeping
to get wrong. ssh's keyboard-interactive has run this arrangement without a
request id for twenty five years.

The failure this appears to invite is a stale submission, where the server moves
on while answers to the previous form are still in flight. Name-keyed answers
already prevent it. Every value travels as its field's name and a value rather
than as a position, so a stale submission cannot land anything in the wrong
field. The server sees names it did not ask for and rejects the submission,
which is the right outcome.

If an identifier is ever genuinely needed, an older reader skips a tag it has
not heard of, so adding one later costs nothing.

The rule governs forms and nothing else. `DENIED` is exempt, and a server may
send it whenever it decides it is done, including while a form is outstanding.
Otherwise a server that wants to stop serving a parked registration has no way
to say so. Read as "the server never speaks unsolicited" the invariant would be
both false and unimplementable.

### Cancelling abandons the method, not the login

A player who clicked "create account", saw a form asking for an email address,
and changed their mind should land back on the login screen with the account
they already had. So `CANCEL` returns the conversation to the point where the
server offered methods, which is exactly where a completed registration returns
it. Registration has one exit, and whether it succeeded or was abandoned makes
no difference to where the conversation resumes.

That scope follows what `nc_auth` already does. Supplying no key means "move on
to the next method" rather than "give up", and cancelling a form means the same.
Giving up entirely is a different answer, given when the server asks which
method to use.

Nothing depends on a cancel arriving. A client that crashes sends none, so the
server has to reach the same end state without one, which means expiry already
covers every case cancellation covers. What cancelling buys is speed, and speed
matters here because the slot it frees is capped per address. The player who
changed their mind stops holding one immediately, and the griefer who opens
registrations in order to abandon them is left paying the full expiry on each.

A cancel can cross a form already in flight, since the server may send one the
moment a wait resolves. A client that has cancelled ignores whatever arrives
until the method offer does.

### Waiting is a state of its own

Between "here is my address" and "here is the code from the mail" the client has
no question to answer and nothing to draw, and a client drawing nothing looks
broken. So the server says so out loud. `WAIT` carries the text to show the
player and moves both ends into a state that is explicitly about waiting for the
external service, and the client stops rendering a form until the server speaks
again.

That message also keeps the server out of trouble. The obvious way to send a
confirmation mail is to send it from inside the code that handles the
submission, which stops that thread while a mail server is thought about. One
thread serves every other connection. `WAIT` is what lets the server start the
slow thing and return immediately, which is the same reason the client side of
`nc_auth` has no callbacks: a state machine that calls out into code which
might wait has a place for a blocking read to hide.

### Choosing is a question for the player

The client machine picks its own order between publickey and password, and can,
because both are attempts to log in as the same person and failing from one to
the next asks nobody anything. Registration is a different intent, and no
ordering rule can infer it: the player pressed "create account" rather than
"log in", and only the player knows that. So it arrives the way every other
human answer does here, by suspending the conversation until the application
supplies it. A server offering only publickey and password suspends nothing and
behaves exactly as it did before.

Usually there is nothing to wait for. A player who clicked "create account" in
the main menu decided before the socket was open, so the answer can be given in
advance and applied the moment the offer lands.

`BEGIN` is a message of its own rather than something folded into `HELLO`, and
the reason is worth more than the message it saves. It can be sent well into
the conversation. A player who mistypes a password, fails, and works out they
never had an account on this shard registers from where they are, without
dropping the session and starting over. Folding the intent into the opening
message would make registration something you can only ask for in the first
breath, and it would invert the server's order of work, since the callback that
decides what to offer is given the name first.

Registration being a bit in `METHODS` is also what lets a client draw its own
login screen. The alternative is a single interactive method whose first form
asks "log in, or create an account", which is simpler in every way except the
one that matters: it puts a server-drawn choice screen in front of the game's
own branded login for every player, every time, and leaves the client unable to
know whether registration is open before asking. One bit is cheap, and it buys
the client the ability to present registration natively, at the moment the
player expects it.

### Registration returns to the state before it

`DONE` does not authenticate
anyone. It puts the conversation back at the point where the server offered
methods, and the server runs its `methods` callback again, because the answer
has changed: the user now exists. The client then logs in normally, with the
credential it just enrolled.

This costs a few messages and buys a clean separation. There is exactly one path
into an authenticated session, and it is the same one an established player
takes every day.

### Enrolling a key proves possession

If registration carries a client public
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

The form belongs to the operator, which is why it describes itself rather than
being a struct both ends were compiled against. A registration form is
configuration, and two shards of one game are not configured alike: one asks
for a date of birth, one wants an invite code, one runs somewhere that requires
an age check and one does not. A client that has to be rebuilt to see a new
field is a client that cannot follow its own game. The server sends what its
operator wrote, and the client renders whatever arrives.

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
browser has to carry server state through the round trip. Here the state is
keyed by something the server already holds, the session while the conversation
is live and the resumption token across a disconnect, so it never has to leave
the building. A hidden field would only invite a server to trust data it handed
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

Every one of those attributes is a hint. `maxlength`, `min`, `max`, `pattern`,
and `required` exist so the client can catch a typo without a round trip. The
server revalidates
every one of them on arrival and applies its own limits regardless of what the
form declared. A client that ignores the hints entirely is rude, not dangerous.

Text is UTF-8 throughout.

## How a form is encoded

A form is a variable run of variable records, and the IDL has no repeated
field. Teaching the generator one is a large change for a single use, so the
IDL gains a narrower type instead. A `stringlist` is any number of
length-prefixed strings packed back to back inside one ordinary bytes field. It
needs no new wire type, does not touch the tag space, and an older reader that
has never heard of it still skips it as bytes.

Each string carries its own short textual key, so the structure of a record
lives in the strings rather than in a second layer of binary tags:

    t=text  n=email  l=Email address  max=64  req=1
    t=password  n=pw  l=Password  min=8
    t=choice  n=shard  l=Realm  o=Ashen Coast  o=Ravenholt
    t=note  l=We sent a code to the address you gave.

A record begins wherever `t=` appears, so the run is self-delimiting. There is
no nesting and no separator to get wrong. Numbers are decimal text, which costs
a few bytes and buys a form that can be read in a hex dump and that maps almost
line for line onto the file its operator wrote it in. An error is an `e=` entry
inside the record it belongs to. Answers come back the same way, `n=` and `v=`,
with `v=` repeated when a choice allows more than one.

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

The failures come from elsewhere. The player opens the mail on their phone, or
the game crashes, or the wifi drops, and a registration that lived only in the
connection is lost with it. So a pending registration lives on the server, keyed
by a token handed to the client, and a fresh connection can present that token
and carry on where it left off. The token expires, which is where a server sets
how long it is willing to wait, and the number of pending registrations from one
address is capped, so a stranger cannot hold a hundred of them open.

A registration that ends without finishing does so in three ways, and all three
have to converge on the same cleanup or a half-made account outlives the attempt
that made it. The client cancels. The connection goes away, through a crash, a
lost network, a refusal, or the application tearing the session down. Or the
token's clock runs out, long after the connection it was issued on stopped
existing.

The first two arrive through `nc_auth`, which tells the application to drop what
was pending. The third cannot, because by then there is no conversation left to
tell anyone anything. Expiry is the server's own clock over its own store, and
netchan has nothing to say about it beyond insisting it exists: a server that
cleans up only on cancel leaks every registration a player ever walked away
from.

Invalidating the token is the part that matters most. Mail already sent cannot
be recalled, so a player who cancels and then clicks the link that arrived
anyway has to find it dead. And whichever path ends the conversation, the
submitted form is wiped rather than merely forgotten, because it has a plaintext
password sitting in it.

A one-time login token is a bearer credential, for the case where a player logs
in through a link in an email or a code sent by SMS. Holding it is being the
user. Its security is exactly the security of the channel that delivered it: an
emailed token is as safe as the player's mailbox. Such tokens are single use and
short lived.

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
