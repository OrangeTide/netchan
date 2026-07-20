# Examples

None of this is part of the library. Build without it:

```sh
make NETCHAN_EXAMPLES=0
```

Binaries land in `_out/<triplet>/bin/`. The rest of this page assumes that
directory is on your `PATH`:

```sh
export PATH="$PWD/_out/$(uname -m)-linux-gnu/bin:$PATH"
```

## chat -- plain UDP

The smallest complete thing: open a connection, open a channel, move bytes.
No crypto, no login, no event loop.

```sh
netchan_example server 9000
netchan_example client 127.0.0.1 9000
```

## ws_gateway -- a browser on the same server as a native client

The browser cannot open a UDP socket, so the gateway terminates its WebSocket
and relays the datagrams on. Each browser gets its own UDP socket toward the
server and arrives there as an ordinary peer, which is why the server needs no
changes at all.

```sh
ws_gateway [ws_port] [game_host] [game_port] [docroot]
```

It also serves static files, to save running a second thing during
development.

## echo -- an encrypted session

`secure_link` wires a netchan connection, an `nc_crypto` decorator under it,
and an iox loop that owns the socket, the retransmit timer, and the signals.
The crypto handshake always completes before netchan's own, so even netchan's
SYN travels sealed. There is no authentication here: this is the NN handshake,
secrecy against a passive listener and nothing more.

```sh
echo_server --port 9000
echo_client --port 9000
```

## auth -- host keys, known_hosts, and a login

Four layers, with the event loop underneath all of them:

```
  nc_auth      who the client is           messages on a reliable channel
  netchan      reliable ordered delivery   the protocol core
  nc_crypto    secrecy and server identity a transport decorator
  iox          socket readiness, timers, signals
```

Each is ignorant of the others. netchan does not know it is encrypted;
`nc_crypto` does not know a login is happening above it; `nc_auth` does not
know what carries its messages. `auth_link.c` is the only file that holds all
four in view, and it is short because the seams are real.

The programs create key files in the current directory, so work somewhere
disposable:

```sh
mkdir /tmp/nc-auth && cd /tmp/nc-auth
```

### A key login

```sh
nc_keygen -f id_netchan
```

It asks for a passphrase. Press Enter twice to leave the key unencrypted, or
type one to have the secret sealed with XChaCha20-Poly1305 under an Argon2id
key. Either way it prints the line to enrol on the server:

```sh
echo "alice 3d92b22b...dcc1" > authorized_keys
auth_server --port 9000
```

On its first run the server generates `host_key`, its long-term identity, and
prints the public half. That file is the thing clients remember. Then, in
another terminal in the same directory:

```sh
auth_client --port 9000 --user alice
```

First contact records the host key and logs in with the key pair. Connect a
second time and the host-key lines are gone: the key matched what was on file,
so there was nothing to report.

### A password login

```sh
auth_server --adduser bob                       # prompts twice, Argon2id
auth_client --port 9000 --user bob --key /nonexistent
```

The client offers public key first, finds no key file, and falls back to the
password prompt. `--password` skips the key even when one exists. The server
spends the same Argon2 work on an account that does not exist as on one that
does, so a stopwatch cannot be used to find out which names are real.

### The host key changing under you

This is the warning the whole `known_hosts` scheme exists to produce.

```sh
mv host_key host_key.orig
auth_server --port 9000
```

The client aborts before it sends a username, let alone a password. Put the
original back with `mv host_key.orig host_key`.

### An unauthorised key

```sh
nc_keygen -f id_rogue -N ""
auth_client --port 9000 --user alice --key id_rogue
```

The signature is valid, so the server knows the client really does hold that
secret. It simply is not on the list, so the key is refused and the client
falls back to the password prompt.

## Files the auth demo creates

| File | Side | Contents |
|---|---|---|
| `host_key` | server | the server's X25519 identity secret, mode 0600 |
| `authorized_keys` | server | `<user> <hex Ed25519 public key>` per line |
| `passwd` | server | `<user> <hex salt> <hex Argon2id hash>` per line |
| `known_hosts` | client | `<host> <hex X25519 public key>` per line |
| `id_netchan` | client | the client's Ed25519 key pair, optionally sealed |

All of them are plain text with hex fields, so they can be read, diffed, and
edited by hand. That is deliberate. The trust decisions live in files an
operator controls, not inside the protocol.

## What these demos are not

- **One client at a time.** `auth_link` holds a single netchan connection. A
  real server would key sessions by source address and connection id.
- **No rate limiting.** Six failed attempts end a session, but nothing stops a
  client reconnecting immediately.
- **No revocation with a deadline.** Removing a line from `authorized_keys`
  denies the next login, not a session already running.
- **First contact is unauthenticated.** The client records an unknown host key
  rather than prompting, which is ssh's `StrictHostKeyChecking=accept-new`. An
  attacker present for that one connection gets its own key pinned. Checking
  the fingerprint out of band is the only real fix, and it is a policy the
  `verify_peer` callback is free to implement.
