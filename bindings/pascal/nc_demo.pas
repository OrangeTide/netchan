{ nc_demo : a walkthrough of the netchan Free Pascal binding, and its smoke test.

  Each section does one job and checks its result. Every check prints one
  line. The program exits 0 when every check passed, and 1 otherwise.

  The first section mirrors examples/typed/typed_example.c. Two connections
  are wired together by a pump procedure instead of a socket. The other
  sections exercise crypto, beacons, and auth. }

{$mode objfpc}{$H+}

program nc_demo;

uses
  ctypes, netchan;

var
  failures: longint;
  clockMs: cuint32;

{ Monotonic demo clock. Each call advances it by one tick, which satisfies the
  clock contract because the three timed calls never see time move backward. }
function NextNowMs: cuint32;
begin
  Inc(clockMs);
  NextNowMs := clockMs;
end;

{ Print one line for a check and count a failure when it does not hold. }
procedure Check(cond: boolean; const what: string);
begin
  if cond then
    writeln('  ok    ', what)
  else begin
    writeln('  FAIL  ', what);
    Inc(failures);
  end;
end;

{ Build a loopback address. Here it is only a routing token, nothing is opened. }
function MakeAddr(port: cuint16): TNcbAddr;
begin
  Result := ncb_addr_new;
  if NcbAddrFrom(Result, '127.0.0.1', port) <> NCB_OK then
    Inc(failures);
end;

{ Move every datagram src has queued over to dst. srcAddr is the transport
  address the datagrams come from, as dst sees it. scratch receives the
  destination address that send_next reports, which the pump ignores. }
procedure Pump(src, dst: TNcbConn; srcAddr, scratch: TNcbAddr);
var
  pkt: RawByteString;
begin
  pkt := NcbSendNext(src, scratch, NextNowMs);
  while pkt <> '' do begin
    NcbFeedBytes(dst, pkt, srcAddr, NextNowMs);
    pkt := NcbSendNext(src, scratch, NextNowMs);
  end;
end;

{ Several rounds in both directions, so acks and handshakes settle. }
procedure PumpBoth(a, b: TNcbConn; aAddr, bAddr, scratch: TNcbAddr);
var
  i: longint;
begin
  for i := 1 to 8 do begin
    Pump(a, b, aAddr, scratch);
    Pump(b, a, bAddr, scratch);
  end;
end;

{ Discard queued events. The demo does not need them all. }
procedure DrainEvents(c: TNcbConn; ev: TNcbEvent);
begin
  while ncb_poll(c, ev) = 1 do begin
  end;
end;

{ ---- 1. Core: connect, open a channel, send a message ---- }

procedure CoreDemo;
var
  client, server: TNcbConn;
  caddr, saddr, scratch: TNcbAddr;
  ev: TNcbEvent;
  tx, rx, evCh: TNcbChan;
  sent, received, chunk: RawByteString;
  contentType: string;
begin
  writeln('1. core: two connections, a reliable channel, one message');

  client := ncb_open(0);
  server := ncb_open(1);
  Check((client <> nil) and (server <> nil), 'opened a client and a server connection');
  if (client = nil) or (server = nil) then begin
    if client <> nil then ncb_close(client);
    if server <> nil then ncb_close(server);
    exit;
  end;

  caddr := MakeAddr(10000);
  saddr := MakeAddr(20000);
  scratch := ncb_addr_new;
  ev := ncb_event_new;

  { Handshake: the client connects, the server accepts, then both sides settle. }
  ncb_connect(client, saddr);
  Pump(client, server, caddr, scratch);
  ncb_accept(server);
  PumpBoth(client, server, caddr, saddr, scratch);
  DrainEvents(client, ev);
  DrainEvents(server, ev);
  Check(ncb_state(client) = NETCHAN_STATE_CONNECTED, 'client reached CONNECTED');
  Check(ncb_state(server) = NETCHAN_STATE_CONNECTED, 'server reached CONNECTED');

  { Open a reliable send channel on the client. Its content type is the topic. }
  tx := ncb_chan_open(client, NETCHAN_RELIABLE, NETCHAN_DIR_SEND, 'chat');
  Check(tx <> nil, 'client opened a reliable send channel "chat"');
  PumpBoth(client, server, caddr, saddr, scratch);

  { The server learns of the channel from a CHAN_OPEN event. }
  rx := nil;
  while ncb_poll(server, ev) = 1 do
    if ncb_event_type(ev) = NETCHAN_EV_CHAN_OPEN then
      rx := ncb_event_chan(ev);
  Check(rx <> nil, 'server saw the channel open');
  if rx <> nil then begin
    contentType := string(ncb_chan_content_type(rx));
    Check(contentType = 'chat', 'content type crossed with the channel');
  end;

  { Write one message, deliver it, then drain the server's DATA events. }
  sent := 'hello, netchan';
  Check(NcbChanWrite(tx, sent) = Length(sent), 'client queued the message');
  PumpBoth(client, server, caddr, saddr, scratch);

  received := '';
  while ncb_poll(server, ev) = 1 do
    if ncb_event_type(ev) = NETCHAN_EV_DATA then begin
      evCh := ncb_event_chan(ev);
      if evCh <> nil then
        repeat
          chunk := NcbChanRead(evCh);
          received := received + chunk;
        until chunk = '';
    end;
  Check(received = sent, 'server received the bytes that were sent');

  { Free what this section created. }
  ncb_chan_close(tx);
  ncb_event_free(ev);
  ncb_addr_free(scratch);
  ncb_addr_free(caddr);
  ncb_addr_free(saddr);
  ncb_close(client);
  ncb_close(server);
end;

{ ---- 2. Crypto: handshake through open(), then seal and open ---- }

procedure CryptoDemo;
var
  ca, cb: TNcbCrypto;
  hp, plain, sealed, got: RawByteString;
  sidA, sidB: array[0..31] of cuint8;
  i: longint;
  n: clong;
begin
  writeln('2. crypto: handshake, then a sealed round trip');

  ca := ncb_crypto_new;
  cb := ncb_crypto_new;
  Check(ncb_crypto_init(ca, 0, nil) = NC_CRYPTO_OK, 'side 0 initialised (connecting role)');
  Check(ncb_crypto_init(cb, 1, nil) = NC_CRYPTO_OK, 'side 1 initialised (accepting role)');

  { Each side's HELLO goes to the other through open(). A HELLO opens to zero
    bytes, so the handshake needs no delivery of its own. }
  for i := 1 to 4 do begin
    hp := NcbCryptoHandshake(ca);
    if hp <> '' then NcbCryptoOpen(cb, hp, got);
    hp := NcbCryptoHandshake(cb);
    if hp <> '' then NcbCryptoOpen(ca, hp, got);
  end;

  Check(ncb_crypto_ready(ca) = 1, 'side 0 is ready');
  Check(ncb_crypto_ready(cb) = 1, 'side 1 is ready');

  if (ncb_crypto_ready(ca) = 1) and (ncb_crypto_ready(cb) = 1) then begin
    ncb_crypto_session_id(ca, @sidA[0]);
    ncb_crypto_session_id(cb, @sidB[0]);
    Check(CompareByte(sidA, sidB, 32) = 0, 'both sides agree on the session id');

    plain := 'attack at dawn';
    NcbCryptoSeal(ca, plain, sealed);
    n := NcbCryptoOpen(cb, sealed, got);
    Check((n = Length(plain)) and (got = plain), 'side 0 to side 1 round trip');

    plain := 'acknowledged';
    NcbCryptoSeal(cb, plain, sealed);
    n := NcbCryptoOpen(ca, sealed, got);
    Check((n = Length(plain)) and (got = plain), 'side 1 to side 0 round trip');
  end;

  ncb_crypto_free(ca);
  ncb_crypto_free(cb);
end;

{ ---- 3. Beacon: set fields, build, parse back ---- }

procedure BeaconDemo;
var
  b1, b2: TNcbBeacon;
  pkt: RawByteString;
  key1, key2: array[0..31] of cuint8;
  i: longint;
  n: clong;
begin
  writeln('3. beacon: build an announce, parse it into a second beacon');

  b1 := ncb_beacon_new;
  b2 := ncb_beacon_new;

  ncb_beacon_set_game(b1, $1234ABCD);
  ncb_beacon_set_wire(b1, 3);
  ncb_beacon_set_version(b1, 1, 2);
  ncb_beacon_set_port(b1, 7777);
  ncb_beacon_set_players(b1, 2, 8);
  ncb_beacon_set_modes(b1, 5);
  ncb_beacon_set_flags(b1, NC_BEACON_F_CREDENTIALS);
  ncb_beacon_set_instance(b1, 99);
  ncb_beacon_set_name(b1, 'demo server');
  ncb_beacon_set_contact(b1, 'admin');
  for i := 0 to 31 do
    key1[i] := i + 1;
  ncb_beacon_set_key(b1, @key1[0]);

  n := NcbBeaconBuild(b1, pkt);
  Check(n > 0, 'built a beacon packet');
  Check((n > 0) and (ncb_beacon_kind(Pointer(pkt), Length(pkt)) = NC_BEACON_ANNOUNCE),
        'packet is an ANNOUNCE');

  Check(ncb_beacon_parse(b2, Pointer(pkt), Length(pkt)) = NC_BEACON_OK, 'parsed the packet');
  Check((ncb_beacon_game(b2) = $1234ABCD) and (ncb_beacon_wire(b2) = 3),
        'game and wire fields match');
  Check((ncb_beacon_major(b2) = 1) and (ncb_beacon_minor(b2) = 2) and (ncb_beacon_port(b2) = 7777),
        'version and port fields match');
  Check((ncb_beacon_players(b2) = 2) and (ncb_beacon_capacity(b2) = 8),
        'player count and capacity match');
  Check((ncb_beacon_modes(b2) = 5) and (ncb_beacon_flags(b2) = NC_BEACON_F_CREDENTIALS)
        and (ncb_beacon_instance(b2) = 99), 'modes, flags and instance match');
  Check((string(ncb_beacon_name(b2)) = 'demo server') and (string(ncb_beacon_contact(b2)) = 'admin'),
        'name and contact match');
  Check(ncb_beacon_key(b2, @key2[0]) = 1, 'the server key is present');
  Check(CompareByte(key1, key2, 32) = 0, 'the server key matches');

  ncb_beacon_free(b1);
  ncb_beacon_free(b2);
end;

{ ---- 4. Auth: framer round trip, signed digest ---- }

procedure AuthDemo;
var
  a: TNcbAuth;
  f: TNcbFramer;
  msg, framed, got: RawByteString;
  sid, pk, d1, d2, d3: array[0..31] of cuint8;
  i: longint;
  n: clong;
begin
  writeln('4. auth: framed messages and the signed digest');

  a := ncb_auth_new;
  Check(a <> nil, 'created an auth object');

  msg := 'login step one';
  framed := NcbFrame(msg);
  Check(Length(framed) = Length(msg) + 2, 'frame adds a two-byte length prefix');

  f := ncb_framer_new(512);
  Check(f <> nil, 'created a framer');
  if f <> nil then begin
    Check(ncb_framer_push(f, Pointer(framed), Length(framed)) = NC_AUTH_OK,
          'pushed one framed message');
    n := NcbFramerNext(f, got);
    Check((n = Length(msg)) and (got = msg), 'framer returns the original message');
    n := NcbFramerNext(f, got);
    Check(n = 0, 'no further message is pending');
    ncb_framer_free(f);
  end;

  for i := 0 to 31 do begin
    sid[i] := 9;
    pk[i] := 1;
  end;
  ncb_auth_signed_digest(@d1[0], @sid[0], 'alice', @pk[0]);
  ncb_auth_signed_digest(@d2[0], @sid[0], 'alice', @pk[0]);
  ncb_auth_signed_digest(@d3[0], @sid[0], 'bob', @pk[0]);
  Check(CompareByte(d1, d2, 32) = 0, 'signed digest is deterministic');
  Check(CompareByte(d1, d3, 32) <> 0, 'signed digest depends on the user name');

  ncb_auth_free(a);
end;

{ ---- Main ---- }

begin
  failures := 0;
  clockMs := 0;

  writeln('netchan ', string(ncb_version_string), ': Free Pascal binding walkthrough');
  Check(ncb_version() > 0, 'shim reports a version');

  CoreDemo;
  CryptoDemo;
  BeaconDemo;
  AuthDemo;

  writeln;
  if failures > 0 then begin
    writeln(failures, ' check(s) failed');
    Halt(1);
  end;
  writeln('ok: core, crypto, beacon and auth checks passed');
end.
