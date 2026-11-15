-- test.lua : smoke test for the netchan Lua binding
--
-- Made by a machine.
--
-- Run with NCDIR set to the directory that holds libnetchan.so. Each check
-- prints one line. The script exits 0 when every check passes, else 1.

package.cpath = (os.getenv("NCDIR") or ".") .. "/lib?.so;" .. package.cpath
local nc = require("netchan")

local checks = 0
local failures = 0

local function check(cond, label)
    checks = checks + 1
    if cond then
        print("  ok    " .. label)
    else
        failures = failures + 1
        print("  FAIL  " .. label)
    end
    return cond
end

-- The shim wants a clock that never moves backward. Each call advances it.
local clock = 0
local function now()
    clock = clock + 5
    return clock
end

-- Move every queued packet from one connection to the other.
local function pump(from, to, from_addr)
    while true do
        local data = from:send_next(now())
        if not data then
            break
        end
        to:feed(data, from_addr, now())
    end
end

-- Pump both ways a few times, so acknowledgements also cross.
local function pump_both(a, b, a_addr, b_addr)
    for _ = 1, 8 do
        pump(a, b, a_addr)
        pump(b, a, b_addr)
    end
end

-- Pop every queued event and return how many there were.
local function drain(conn, ev)
    local n = 0
    while conn:poll(ev) do
        n = n + 1
    end
    return n
end

local function section(name, fn)
    print(name)
    local ok, err = pcall(fn)
    if not ok then
        failures = failures + 1
        print("  FAIL  raised an error: " .. tostring(err))
    end
end

---------------------------------------------------------------------------
-- 1. Core: handshake, channel, one message end to end
---------------------------------------------------------------------------

section("core", function()
    local caddr = nc.addr("127.0.0.1", 10000)
    local saddr = nc.addr("127.0.0.1", 20000)
    local client = nc.open(0)
    local server = nc.open(1)
    local ev = nc.event()

    check(client:connect(saddr) == nc.OK, "client connect is accepted")
    pump(client, server, caddr)
    check(server:accept() == nc.OK, "server accepts the connection")
    pump_both(client, server, caddr, saddr)
    drain(client, ev)
    drain(server, ev)
    check(client:state() == nc.STATE_CONNECTED, "client reaches CONNECTED")
    check(server:state() == nc.STATE_CONNECTED, "server reaches CONNECTED")

    local tx = client:chan_open(nc.RELIABLE, nc.DIR_SEND, "chat")
    check(tx ~= nil, "client opens a reliable send channel")
    check(tx:content_type() == "chat", "client channel reports its content type")
    pump_both(client, server, caddr, saddr)

    local rx = nil
    while server:poll(ev) do
        if ev:type() == nc.EV_CHAN_OPEN then
            rx = ev:chan()
        end
    end
    check(rx ~= nil, "server sees the channel open")
    check(rx ~= nil and rx:content_type() == "chat",
          "server channel carries the content type")

    local msg = "hello, netchan"
    check(tx:write(msg) == #msg, "client writes the message")
    pump_both(client, server, caddr, saddr)

    local got = nil
    local data_events = 0
    while server:poll(ev) do
        if ev:type() == nc.EV_DATA then
            data_events = data_events + 1
            local chan = ev:chan()
            local bytes = chan:read()
            if bytes ~= nil then
                got = bytes
            end
        end
    end
    check(data_events >= 1, "server gets a DATA event")
    check(got == msg, "message arrived intact (got '" .. tostring(got) .. "')")

    local stats = client:conn_stats()
    check(type(stats.pkts_sent) == "number" and stats.pkts_sent > 0,
          "connection stats count sent packets")
    client:close()
    server:close()
end)

---------------------------------------------------------------------------
-- 2. Crypto: two roles, handshake, seal and open both ways
---------------------------------------------------------------------------

section("crypto", function()
    local a = nc.crypto()
    local b = nc.crypto()
    a:init(0)
    b:init(1)

    local ha = a:handshake_packet()
    local hb = b:handshake_packet()
    check(#ha == nc.CRYPTO_HELLO_LEN, "initiator HELLO has the fixed length")

    check(b:open(ha) == nil, "responder consumes the initiator HELLO")
    check(a:open(hb) == nil, "initiator consumes the responder HELLO")
    check(a:ready() and b:ready(), "both sides are ready after the handshake")
    check(not a:failed() and not b:failed(), "neither side failed")
    check(a:session_id() ~= nil and a:session_id() == b:session_id(),
          "both sides hold the same session id")

    local plain = "the quick brown fox"
    local sealed = a:seal(plain)
    check(b:open(sealed) == plain, "a sealed message opens on the peer")

    local reply = "jumps over the lazy dog"
    check(a:open(b:seal(reply)) == reply, "responder's reply opens on the initiator")
end)

---------------------------------------------------------------------------
-- 3. Beacon: build with fields set, parse back, compare
---------------------------------------------------------------------------

section("beacon", function()
    local sk = string.rep("\7", 32)
    local pk = nc.crypto_identity_public(sk)
    check(#pk == 32, "identity public key is 32 bytes")

    local b = nc.beacon()
    b:set_game(0xC0FFEE)
    b:set_wire(3)
    b:set_version(1, 4)
    b:set_port(20000)
    b:set_players(2, 8)
    b:set_modes(5)
    b:set_flags(nc.BEACON_F_CREDENTIALS)
    b:set_instance(42)
    b:set_name("Test Server")
    b:set_contact("ops")
    b:set_key(pk)

    local wire = b:build()
    check(nc.beacon_kind(wire) == nc.BEACON_ANNOUNCE, "built beacon is an announce")

    local back = nc.beacon()
    check(back:parse(wire), "beacon parses back")
    check(back:game() == 0xC0FFEE, "game survives the round trip")
    check(back:wire() == 3, "wire version survives")
    check(back:major() == 1 and back:minor() == 4, "game version survives")
    check(back:port() == 20000, "port survives")
    check(back:players() == 2 and back:capacity() == 8, "player counts survive")
    check(back:modes() == 5, "modes survive")
    check(back:flags() == nc.BEACON_F_CREDENTIALS, "flags survive")
    check(back:instance() == 42, "instance survives")
    check(back:name() == "Test Server", "name survives")
    check(back:contact() == "ops", "contact survives")
    check(back:key() == pk, "identity key survives")

    local probe = nc.beacon_probe(0xC0FFEE)
    check(nc.beacon_kind(probe) == nc.BEACON_PROBE, "probe is a probe")
    check(nc.beacon_probe_game(probe) == 0xC0FFEE, "probe carries its game")
end)

---------------------------------------------------------------------------
-- 4. Auth and keystore: framing, digest, object construction, key files
---------------------------------------------------------------------------

section("auth", function()
    local sid = string.rep("\1", 32)
    local pk = nc.crypto_identity_public(string.rep("\9", 32))

    local client = nc.auth()
    client:client_init(sid, "alice", function(_) end)
    check(client:user() == "alice", "client auth object names its user")

    local server = nc.auth()
    server:server_init(sid, {
        methods = function(_) return nc.AUTH_M_PUBKEY end,
        check_key = function(_, _) return true end,
    }, function(_) end)
    check(server:state() == nc.AUTH_STATE_PENDING, "server auth conversation is pending")

    local framed = nc.frame("hello, auth")
    local fr = nc.framer(1024)
    check(fr:push(framed), "framer accepts a framed message")
    check(fr:next() == "hello, auth", "framer returns the original message")
    check(fr:next() == nil, "framer has nothing left after one message")

    local small = nc.framer(8)
    check(not small:push(string.rep("x", 20)), "framer refuses bytes past its capacity")

    local d1 = nc.auth_signed_digest(sid, "alice", pk)
    local d2 = nc.auth_signed_digest(sid, "alice", pk)
    local d3 = nc.auth_signed_digest(sid, "bob", pk)
    check(#d1 == 32, "signed digest is 32 bytes")
    check(d1 == d2, "signed digest is deterministic")
    check(d1 ~= d3, "signed digest depends on the user")

    local hex = nc.hex_encode("\0\1\255")
    check(hex == "0001ff", "hex_encode produces lowercase hex")
    check(nc.hex_decode(hex) == "\0\1\255", "hex_decode reverses hex_encode")

    local path = os.tmpname()
    os.remove(path)
    local made_pk = nc.keyfile_generate(path, "pw")
    check(#made_pk == 32, "keyfile_generate returns a public key")
    check(nc.keyfile_encrypted(path), "a passphrase keyfile reports encrypted")
    local sk2, pk2 = nc.keyfile_load(path, "pw")
    check(#sk2 == 64 and pk2 == made_pk, "keyfile_load returns the stored key pair")
    os.remove(path)
end)

---------------------------------------------------------------------------

print(string.format("\n%d checks, %d failure(s)", checks, failures))
os.exit(failures == 0 and 0 or 1)
