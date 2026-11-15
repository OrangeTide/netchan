-- example.lua : a walkthrough of the netchan Lua binding
--
-- Made by a machine.
--
-- The script runs two connections in one process, with no sockets. A "pump"
-- moves each packet from one side's send_next into the other side's feed.
-- A real program would move the same bytes over a UDP socket instead.
--
-- Usage: NCDIR=/path/to/dir/with/libnetchan.so lua example.lua

local ndir = os.getenv("NCDIR")
if ndir then
    package.cpath = ndir .. "/lib?.so;" .. package.cpath
else
    -- Fall back to the usual location in the build tree, relative to here.
    package.cpath = "../../_out/x86_64-linux-gnu/shared/lib/lib?.so;" .. package.cpath
end
local nc = require("netchan")

-- Time for netchan is a number of milliseconds that never moves backward.
local clock = 0
local function now()
    clock = clock + 5
    return clock
end

-- Move everything one side has queued to the other side.
local function pump(from, to, from_addr)
    while true do
        local packet = from:send_next(now())
        if not packet then
            break
        end
        to:feed(packet, from_addr, now())
    end
end

-- Several rounds in both directions let acknowledgements settle.
local function pump_both(a, b, a_addr, b_addr)
    for _ = 1, 8 do
        pump(a, b, a_addr)
        pump(b, a, b_addr)
    end
end

-- Pop every queued event on a connection, calling handle for each one.
local function each_event(conn, ev, handle)
    while conn:poll(ev) do
        handle(ev)
    end
end

---------------------------------------------------------------------------
print("== 1. A connection and a message\n")

-- Each side has an address. The client connects to the server's address.
local client_addr = nc.addr("127.0.0.1", 10000)
local server_addr = nc.addr("127.0.0.1", 20000)

-- open(0) makes the connecting side. open(1) makes the accepting side.
local client = nc.open(0)
local server = nc.open(1)
local ev = nc.event()

-- Handshake: connect, carry the packets across, then accept.
client:connect(server_addr)
pump(client, server, client_addr)
server:accept()
pump_both(client, server, client_addr, server_addr)

-- Connection events (CONNECTED and so on) are queued. Discard them here.
each_event(client, ev, function() end)
each_event(server, ev, function() end)
print("connected:", client:state() == nc.STATE_CONNECTED
      and server:state() == nc.STATE_CONNECTED)

-- A channel carries an ordered, reliable stream of messages. The content
-- type is a label both sides can see; "chat" is ours to pick.
local tx = client:chan_open(nc.RELIABLE, nc.DIR_SEND, "chat")
pump_both(client, server, client_addr, server_addr)

-- The server learns about the channel from a CHAN_OPEN event.
local rx = nil
each_event(server, ev, function()
    if ev:type() == nc.EV_CHAN_OPEN then
        rx = ev:chan()
    end
end)
print("server sees channel with content type:", rx and rx:content_type())

-- Write three messages. Each write is one message on the channel.
local lines = { "hello", "netchan from Lua", "bye" }
for _, line in ipairs(lines) do
    tx:write(line)
end
pump_both(client, server, client_addr, server_addr)

-- Data arrives as DATA events. Each one names the channel that has bytes.
-- read() returns one message at a time, or nil when none is waiting.
each_event(server, ev, function()
    if ev:type() == nc.EV_DATA then
        local chan = ev:chan()
        local msg = chan:read()
        while msg do
            print("server received:", msg)
            msg = chan:read()
        end
    end
end)

client:close()
server:close()

---------------------------------------------------------------------------
print("\n== 2. Encrypted sessions\n")

-- Role 0 initiates and role 1 responds. Each side sends one HELLO packet
-- and feeds the peer's HELLO to open(). open() returns nil for a HELLO
-- because a handshake packet carries no payload.
local alice = nc.crypto()
local bob = nc.crypto()
alice:init(0)
bob:init(1)

bob:open(alice:handshake_packet())
alice:open(bob:handshake_packet())
print("both ready:", alice:ready() and bob:ready())

-- seal() turns plaintext into a packet. open() on the peer returns it.
local sealed = alice:seal("meet at the bridge")
print("sealed bytes:", #sealed)
print("bob reads:", bob:open(sealed))

---------------------------------------------------------------------------
print("\n== 3. Discovery beacon\n")

-- A server advertises itself with a beacon. A browser parses it.
local advert = nc.beacon()
advert:set_game(0xC0FFEE)
advert:set_version(1, 4)
advert:set_port(20000)
advert:set_players(3, 16)
advert:set_name("Example Server")
advert:set_flags(nc.BEACON_F_CREDENTIALS)
local wire = advert:build()

local seen = nc.beacon()
if seen:parse(wire) then
    print(string.format("name=%s players=%d/%d version=%d.%d",
                        seen:name(), seen:players(), seen:capacity(),
                        seen:major(), seen:minor()))
end

-- A browser can ask who is out there, and filter on the game id.
local probe = nc.beacon_probe(0xC0FFEE)
print("probe kind:", nc.beacon_kind(probe) == nc.BEACON_PROBE and "probe" or "other")
print("probe game matches:", nc.beacon_probe_game(probe) == 0xC0FFEE)
