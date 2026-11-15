{ netchan : Free Pascal binding for the netchan flat C shim (netchan_bind.h).

  Every ncb_* function in the shim is declared here as a cdecl external
  imported from the shared object libnetchan_c.so. A program that links it
  (fpc -Fl<dir>, with the object on the load path at run time) resolves
  these symbols.

  All netchan objects are opaque handles. Each TNcb* type is a Pointer, and
  no netchan struct layout is needed. Byte buffers are passed as Pointer plus
  csize_t. Out-parameters of C scalar types are Pascal var parameters.

  The helpers after the raw externals wrap the buffer-passing calls with
  RawByteString, so a caller never handles a buffer length by hand. }

{$mode objfpc}{$H+}

unit netchan;

interface

uses
  ctypes;

{ ---- Opaque handles ---- }

type
  TNcbConn       = Pointer;
  TNcbChan       = Pointer;
  TNcbAddr       = Pointer;
  TNcbCfg        = Pointer;
  TNcbEvent      = Pointer;
  TNcbCrypto     = Pointer;
  TNcbCryptoCfg  = Pointer;
  TNcbBeacon     = Pointer;
  TNcbAuth       = Pointer;
  TNcbFramer     = Pointer;
  TNcbForm       = Pointer;
  TNcbFormBuilder = Pointer;
  TNcbValues     = Pointer;

{ The shared object the externals below are imported from. }
const
  NETCHAN_C_LIB = 'netchan_c';

{ ---- Constants ---- }

const
  { Shim return codes }
  NCB_OK  = 0;
  NCB_ERR = -1;

  { Channel types, directions and states (netchan.h) }
  NETCHAN_RELIABLE   = 0;
  NETCHAN_UNRELIABLE = 1;
  NETCHAN_STREAM     = 2;

  NETCHAN_DIR_SEND = 0;
  NETCHAN_DIR_RECV = 1;

  NETCHAN_STATE_NEW        = 0;
  NETCHAN_STATE_CONNECTING = 1;
  NETCHAN_STATE_CONNECTED  = 2;
  NETCHAN_STATE_CLOSING    = 3;
  NETCHAN_STATE_CLOSED     = 4;

  { Event types, as returned by ncb_event_type }
  NETCHAN_EV_NONE        = 0;
  NETCHAN_EV_CONNECTED   = 1;
  NETCHAN_EV_DISCONNECTED = 2;
  NETCHAN_EV_REDIRECT    = 3;
  NETCHAN_EV_CHAN_OPEN   = 4;
  NETCHAN_EV_CHAN_CLOSE  = 5;
  NETCHAN_EV_DATA        = 6;

  { netchan error causes }
  NETCHAN_ERR_NOMEM  = -2;
  NETCHAN_ERR_AGAIN  = -3;
  NETCHAN_ERR_CLOSED = -4;
  NETCHAN_ERR_FLOW   = -5;
  NETCHAN_ERR_PROTO  = -6;
  NETCHAN_ERR_TOOBIG = -7;

  { Crypto (nc_crypto.h) }
  NC_CRYPTO_OK       = 0;
  NC_CRYPTO_ERR      = -1;
  NC_CRYPTO_HELLO    = $01;
  NC_CRYPTO_DATA     = $02;
  NC_CRYPTO_HELLO_LEN = 65;
  NC_CRYPTO_OVERHEAD  = 25;
  NC_CRYPTO_SID_LEN   = 32;

  { Beacon (nc_beacon.h) }
  NC_BEACON_MAGIC        = $3142434E;
  NC_BEACON_MAX          = 512;
  NC_BEACON_MAX_NAME     = 64;
  NC_BEACON_MAX_CONTACT  = 64;
  NC_BEACON_OK           = 0;
  NC_BEACON_ERR          = -1;
  NC_BEACON_ANNOUNCE     = 1;
  NC_BEACON_PROBE        = 2;
  NC_BEACON_F_CREDENTIALS = $01;
  NC_BEACON_F_REGISTER    = $02;

  { Keystore host verdicts (auth/keystore.h) }
  KS_HOST_UNKNOWN = 0;
  KS_HOST_MATCH   = 1;
  KS_HOST_CHANGED = 2;

  { Auth methods (nc_auth.h) }
  NC_AUTH_M_PUBKEY      = $01;
  NC_AUTH_M_PASSWORD    = $02;
  NC_AUTH_M_INTERACTIVE = $04;
  NC_AUTH_M_REGISTER    = $08;

  NC_AUTH_MAX_USER  = 64;
  NC_AUTH_MAX_PASS  = 128;
  NC_AUTH_MAX_MSG   = 256;
  NC_AUTH_MAX_TRIES = 6;

  NC_AUTH_OK  = 0;
  NC_AUTH_ERR = -1;

  NC_FORM_MAX_FIELDS  = 24;
  NC_FORM_MAX_OPTIONS = 16;
  NC_FORM_MAX_NAME    = 32;
  NC_FORM_MAX_LABEL   = 128;
  NC_FORM_MAX_VALUE   = 256;
  NC_FORM_MAX_ERRORS  = 500;
  NC_FORM_MAX_TOKEN   = 64;
  NC_FORM_MAX_MSG     = 2000;

  { Auth conversation state (ncb_auth_state) }
  NC_AUTH_STATE_PENDING = 0;
  NC_AUTH_STATE_OK      = 1;
  NC_AUTH_STATE_DENIED  = 2;

  { What the client is waiting on (ncb_auth_needs) }
  NC_AUTH_NEED_NOTHING  = 0;
  NC_AUTH_NEED_KEY      = 1;
  NC_AUTH_NEED_PASSWORD = 2;
  NC_AUTH_NEED_METHOD   = 3;
  NC_AUTH_NEED_FORM     = 4;

  { Form field types (enum nc_form_type) }
  NC_FF_TEXT     = 1;
  NC_FF_PASSWORD = 2;
  NC_FF_EMAIL    = 3;
  NC_FF_INTEGER  = 4;
  NC_FF_PHONE    = 5;
  NC_FF_BOOL     = 6;
  NC_FF_CHOICE   = 7;
  NC_FF_NOTE     = 8;
  NC_FF_LINK     = 9;

  { Form field flags }
  NC_FF_REQUIRED = $01;
  NC_FF_MULTI    = $02;

  { Interactive server verdicts (NC_AUTH_IA_*) }
  NC_AUTH_IA_MORE   = 0;
  NC_AUTH_IA_DONE   = 1;
  NC_AUTH_IA_DENIED = 2;
  NC_AUTH_IA_WAIT   = 3;

{ ---- Callback types ---- }

type
  { Callbacks take a ctx pointer that the shim hands back unchanged. }
  TNcbSendFn      = procedure(ctx: Pointer; msg: Pointer; len: csize_t); cdecl;
  TNcbMethodsFn   = function(ctx: Pointer; user: PAnsiChar): cuint; cdecl;
  TNcbCheckKeyFn  = function(ctx: Pointer; user: PAnsiChar; pk: PByte): cint; cdecl;
  TNcbCheckPassFn = function(ctx: Pointer; user: PAnsiChar; password: PAnsiChar): cint; cdecl;

  TNcbIaBeginFn     = function(ctx: Pointer; method: cuint; fb: TNcbFormBuilder): cint; cdecl;
  TNcbIaSubmitFn    = function(ctx: Pointer; a: TNcbAuth; fb: TNcbFormBuilder): cint; cdecl;
  TNcbIaResumeFn    = function(ctx: Pointer; tok: PByte; len: csize_t; fb: TNcbFormBuilder): cint; cdecl;
  TNcbIaCancelledFn = procedure(ctx: Pointer); cdecl;

{ ---- Raw externals: version ---- }

function ncb_version_string: PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_version_string';
function ncb_version: cint; cdecl; external NETCHAN_C_LIB name 'ncb_version';

{ ---- Raw externals: addresses ---- }

function ncb_addr_new: TNcbAddr; cdecl; external NETCHAN_C_LIB name 'ncb_addr_new';
procedure ncb_addr_free(a: TNcbAddr); cdecl; external NETCHAN_C_LIB name 'ncb_addr_free';
function ncb_addr_from(a: TNcbAddr; host: PAnsiChar; port: cuint16): cint; cdecl; external NETCHAN_C_LIB name 'ncb_addr_from';
function ncb_addr_text(a: TNcbAddr; out_: PAnsiChar; cap: csize_t; port_out: Pointer): cint; cdecl; external NETCHAN_C_LIB name 'ncb_addr_text';
procedure ncb_addr_copy(dst: TNcbAddr; src: TNcbAddr); cdecl; external NETCHAN_C_LIB name 'ncb_addr_copy';

{ ---- Raw externals: configuration ---- }

function ncb_cfg_new: TNcbCfg; cdecl; external NETCHAN_C_LIB name 'ncb_cfg_new';
procedure ncb_cfg_free(cfg: TNcbCfg); cdecl; external NETCHAN_C_LIB name 'ncb_cfg_free';
procedure ncb_cfg_set(cfg: TNcbCfg; mtu: cuint; chan_window: cuint; idle_timeout_ms: cuint; retransmit_ms: cuint); cdecl; external NETCHAN_C_LIB name 'ncb_cfg_set';
procedure ncb_cfg_get(cfg: TNcbCfg; mtu: Pointer; chan_window: Pointer; idle_timeout_ms: Pointer; retransmit_ms: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_cfg_get';

{ ---- Raw externals: connection lifecycle ---- }

function ncb_open(server: cint): TNcbConn; cdecl; external NETCHAN_C_LIB name 'ncb_open';
procedure ncb_disconnect(c: TNcbConn); cdecl; external NETCHAN_C_LIB name 'ncb_disconnect';
procedure ncb_close(c: TNcbConn); cdecl; external NETCHAN_C_LIB name 'ncb_close';
procedure ncb_config(c: TNcbConn; cfg: TNcbCfg); cdecl; external NETCHAN_C_LIB name 'ncb_config';
function ncb_state(c: TNcbConn): cint; cdecl; external NETCHAN_C_LIB name 'ncb_state';
function ncb_id(c: TNcbConn): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_id';
function ncb_connect(c: TNcbConn; addr: TNcbAddr): cint; cdecl; external NETCHAN_C_LIB name 'ncb_connect';
function ncb_accept(c: TNcbConn): cint; cdecl; external NETCHAN_C_LIB name 'ncb_accept';

{ ---- Raw externals: packet I/O ---- }

function ncb_peek_id(pkt: Pointer; len: csize_t): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_peek_id';
function ncb_feed(c: TNcbConn; pkt: Pointer; len: csize_t; from: TNcbAddr; now_ms: cuint32): cint; cdecl; external NETCHAN_C_LIB name 'ncb_feed';
function ncb_send_next(c: TNcbConn; buf: Pointer; buflen: csize_t; to_: TNcbAddr; now_ms: cuint32): csize_t; cdecl; external NETCHAN_C_LIB name 'ncb_send_next';
function ncb_service(c: TNcbConn; now_ms: cuint32): cint; cdecl; external NETCHAN_C_LIB name 'ncb_service';

{ ---- Raw externals: channels ---- }

function ncb_chan_open(c: TNcbConn; type_: cint; dir: cint; content_type: PAnsiChar): TNcbChan; cdecl; external NETCHAN_C_LIB name 'ncb_chan_open';
procedure ncb_chan_close(ch: TNcbChan); cdecl; external NETCHAN_C_LIB name 'ncb_chan_close';
function ncb_chan_id(ch: TNcbChan): cint; cdecl; external NETCHAN_C_LIB name 'ncb_chan_id';
function ncb_chan_type(ch: TNcbChan): cint; cdecl; external NETCHAN_C_LIB name 'ncb_chan_type';
function ncb_chan_content_type(ch: TNcbChan): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_chan_content_type';
function ncb_chan_state(ch: TNcbChan): cint; cdecl; external NETCHAN_C_LIB name 'ncb_chan_state';
function ncb_chan_write(ch: TNcbChan; data: Pointer; len: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_chan_write';
function ncb_chan_read(ch: TNcbChan; buf: Pointer; buflen: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_chan_read';

{ ---- Raw externals: events ---- }

function ncb_event_new: TNcbEvent; cdecl; external NETCHAN_C_LIB name 'ncb_event_new';
procedure ncb_event_free(ev: TNcbEvent); cdecl; external NETCHAN_C_LIB name 'ncb_event_free';
function ncb_poll(c: TNcbConn; ev: TNcbEvent): cint; cdecl; external NETCHAN_C_LIB name 'ncb_poll';
function ncb_event_type(ev: TNcbEvent): cint; cdecl; external NETCHAN_C_LIB name 'ncb_event_type';
function ncb_event_chan(ev: TNcbEvent): TNcbChan; cdecl; external NETCHAN_C_LIB name 'ncb_event_chan';
function ncb_event_redirect_conn_id(ev: TNcbEvent): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_event_redirect_conn_id';
procedure ncb_event_redirect_addr(ev: TNcbEvent; out_: TNcbAddr); cdecl; external NETCHAN_C_LIB name 'ncb_event_redirect_addr';

{ ---- Raw externals: statistics ---- }

procedure ncb_conn_stats(c: TNcbConn; out_: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_conn_stats';
procedure ncb_chan_stats(ch: TNcbChan; out_: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_chan_stats';

{ ---- Raw externals: crypto ---- }

function ncb_crypto_cfg_new: TNcbCryptoCfg; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_new';
procedure ncb_crypto_cfg_free(cfg: TNcbCryptoCfg); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_free';
procedure ncb_crypto_cfg_set_eph_seed(cfg: TNcbCryptoCfg; seed: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_set_eph_seed';
procedure ncb_crypto_cfg_set_psk(cfg: TNcbCryptoCfg; psk: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_set_psk';
procedure ncb_crypto_cfg_set_static_sk(cfg: TNcbCryptoCfg; sk: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_set_static_sk';
procedure ncb_crypto_cfg_set_require_peer_static(cfg: TNcbCryptoCfg; require: cint); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_cfg_set_require_peer_static';

function ncb_crypto_new: TNcbCrypto; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_new';
procedure ncb_crypto_free(c: TNcbCrypto); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_free';
function ncb_crypto_init(c: TNcbCrypto; role: cint; cfg: TNcbCryptoCfg): cint; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_init';
procedure ncb_crypto_identity_public(out_: PByte; static_sk: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_crypto_identity_public';
function ncb_crypto_handshake_packet(c: TNcbCrypto; out_: PByte; cap: csize_t): csize_t; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_handshake_packet';
function ncb_crypto_ready(c: TNcbCrypto): cint; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_ready';
function ncb_crypto_failed(c: TNcbCrypto): cint; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_failed';
function ncb_crypto_session_id(c: TNcbCrypto; out_: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_session_id';
function ncb_crypto_seal(c: TNcbCrypto; plain: PByte; len: csize_t; out_: PByte; cap: csize_t): clong; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_seal';
function ncb_crypto_open(c: TNcbCrypto; pkt: PByte; len: csize_t; out_: PByte; cap: csize_t): clong; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_open';
function ncb_crypto_peer_static(c: TNcbCrypto; out_: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_crypto_peer_static';

{ ---- Raw externals: discovery (beacon) ---- }

function ncb_beacon_new: TNcbBeacon; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_new';
procedure ncb_beacon_free(b: TNcbBeacon); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_free';

procedure ncb_beacon_set_game(b: TNcbBeacon; game: cuint32); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_game';
function ncb_beacon_game(b: TNcbBeacon): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_game';
procedure ncb_beacon_set_wire(b: TNcbBeacon; wire: cuint16); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_wire';
function ncb_beacon_wire(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_wire';
procedure ncb_beacon_set_version(b: TNcbBeacon; major: cuint16; minor: cuint16); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_version';
function ncb_beacon_major(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_major';
function ncb_beacon_minor(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_minor';
procedure ncb_beacon_set_port(b: TNcbBeacon; port: cuint16); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_port';
function ncb_beacon_port(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_port';
procedure ncb_beacon_set_players(b: TNcbBeacon; players: cuint16; capacity: cuint16); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_players';
function ncb_beacon_players(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_players';
function ncb_beacon_capacity(b: TNcbBeacon): cuint16; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_capacity';
procedure ncb_beacon_set_modes(b: TNcbBeacon; modes: cuint32); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_modes';
function ncb_beacon_modes(b: TNcbBeacon): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_modes';
procedure ncb_beacon_set_flags(b: TNcbBeacon; flags: cuint32); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_flags';
function ncb_beacon_flags(b: TNcbBeacon): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_flags';
procedure ncb_beacon_set_instance(b: TNcbBeacon; instance: cuint32); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_instance';
function ncb_beacon_instance(b: TNcbBeacon): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_instance';

procedure ncb_beacon_set_name(b: TNcbBeacon; name: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_name';
function ncb_beacon_name(b: TNcbBeacon): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_name';
procedure ncb_beacon_set_contact(b: TNcbBeacon; contact: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_contact';
function ncb_beacon_contact(b: TNcbBeacon): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_contact';

procedure ncb_beacon_set_key(b: TNcbBeacon; key: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_beacon_set_key';
function ncb_beacon_key(b: TNcbBeacon; out_: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_key';

function ncb_beacon_build(out_: Pointer; outlen: csize_t; b: TNcbBeacon): clong; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_build';
function ncb_beacon_probe(out_: Pointer; outlen: csize_t; game: cuint32): clong; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_probe';
function ncb_beacon_kind(pkt: Pointer; len: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_kind';
function ncb_beacon_parse(b: TNcbBeacon; pkt: Pointer; len: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_parse';
function ncb_beacon_probe_game(pkt: Pointer; len: csize_t; game: Pointer): cint; cdecl; external NETCHAN_C_LIB name 'ncb_beacon_probe_game';

{ ---- Raw externals: keystore ---- }

procedure ncb_hex_encode(out_: PAnsiChar; in_: PByte; n: csize_t); cdecl; external NETCHAN_C_LIB name 'ncb_hex_encode';
function ncb_hex_decode(out_: PByte; n: csize_t; in_: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_hex_decode';
function ncb_known_host(path: PAnsiChar; host: PAnsiChar; pk: PByte; stored: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_known_host';
function ncb_known_host_add(path: PAnsiChar; host: PAnsiChar; pk: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_known_host_add';
function ncb_host_key(path: PAnsiChar; sk: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_host_key';
function ncb_authorized_key(path: PAnsiChar; user: PAnsiChar; pk: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_authorized_key';
function ncb_check_password(path: PAnsiChar; user: PAnsiChar; password: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_check_password';
function ncb_passwd_add(path: PAnsiChar; user: PAnsiChar; password: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_passwd_add';
function ncb_user_exists(path: PAnsiChar; user: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_user_exists';
function ncb_keyfile_generate(path: PAnsiChar; passphrase: PAnsiChar; pk: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_keyfile_generate';
function ncb_keyfile_load(path: PAnsiChar; passphrase: PAnsiChar; sk: PByte; pk: PByte): cint; cdecl; external NETCHAN_C_LIB name 'ncb_keyfile_load';
function ncb_keyfile_encrypted(path: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_keyfile_encrypted';

{ ---- Raw externals: auth ---- }

function ncb_auth_new: TNcbAuth; cdecl; external NETCHAN_C_LIB name 'ncb_auth_new';
procedure ncb_auth_free(a: TNcbAuth); cdecl; external NETCHAN_C_LIB name 'ncb_auth_free';

procedure ncb_auth_client_init(a: TNcbAuth; sid: PByte; user: PAnsiChar; send: TNcbSendFn; send_ctx: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_auth_client_init';
procedure ncb_auth_server_init(a: TNcbAuth; sid: PByte; methods: TNcbMethodsFn; check_key: TNcbCheckKeyFn; check_password: TNcbCheckPassFn; cb_ctx: Pointer; send: TNcbSendFn; send_ctx: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_auth_server_init';
procedure ncb_auth_server_interactive(a: TNcbAuth; buflen: csize_t; begin_: TNcbIaBeginFn; submit: TNcbIaSubmitFn; resume: TNcbIaResumeFn; cancelled: TNcbIaCancelledFn; ctx: Pointer); cdecl; external NETCHAN_C_LIB name 'ncb_auth_server_interactive';
procedure ncb_auth_client_buffer(a: TNcbAuth; buflen: csize_t); cdecl; external NETCHAN_C_LIB name 'ncb_auth_client_buffer';

procedure ncb_auth_start(a: TNcbAuth); cdecl; external NETCHAN_C_LIB name 'ncb_auth_start';
function ncb_auth_feed(a: TNcbAuth; msg: Pointer; len: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_feed';
function ncb_auth_needs(a: TNcbAuth): cint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_needs';
procedure ncb_auth_supply_key(a: TNcbAuth; sk: PByte; pk: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_auth_supply_key';
procedure ncb_auth_supply_password(a: TNcbAuth; password: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_auth_supply_password';
function ncb_auth_state(a: TNcbAuth): cint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_state';
function ncb_auth_user(a: TNcbAuth): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_auth_user';
procedure ncb_auth_signed_digest(out_: PByte; sid: PByte; user: PAnsiChar; pk: PByte); cdecl; external NETCHAN_C_LIB name 'ncb_auth_signed_digest';
procedure ncb_auth_clear(a: TNcbAuth); cdecl; external NETCHAN_C_LIB name 'ncb_auth_clear';

procedure ncb_auth_supply_method(a: TNcbAuth; method: cuint); cdecl; external NETCHAN_C_LIB name 'ncb_auth_supply_method';
function ncb_auth_offered(a: TNcbAuth): cuint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_offered';
function ncb_auth_waiting(a: TNcbAuth): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_auth_waiting';
function ncb_auth_registered(a: TNcbAuth): cint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_registered';
function ncb_auth_pending_token(a: TNcbAuth; out_: PByte; len: csize_t): csize_t; cdecl; external NETCHAN_C_LIB name 'ncb_auth_pending_token';
procedure ncb_auth_resume_pending(a: TNcbAuth; tok: PByte; len: csize_t); cdecl; external NETCHAN_C_LIB name 'ncb_auth_resume_pending';
procedure ncb_auth_cancel(a: TNcbAuth); cdecl; external NETCHAN_C_LIB name 'ncb_auth_cancel';

function ncb_auth_form(a: TNcbAuth): TNcbForm; cdecl; external NETCHAN_C_LIB name 'ncb_auth_form';

{ ---- Raw externals: form reading ---- }

function ncb_form_title(f: TNcbForm): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_title';
function ncb_form_note(f: TNcbForm): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_note';
function ncb_form_nfields(f: TNcbForm): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_nfields';
function ncb_form_field_type(f: TNcbForm; i: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_type';
function ncb_form_field_flags(f: TNcbForm; i: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_flags';
function ncb_form_field_name(f: TNcbForm; i: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_name';
function ncb_form_field_label(f: TNcbForm; i: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_label';
function ncb_form_field_value(f: TNcbForm; i: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_value';
function ncb_form_field_pattern(f: TNcbForm; i: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_pattern';
function ncb_form_field_error(f: TNcbForm; i: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_error';
function ncb_form_field_min(f: TNcbForm; i: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_min';
function ncb_form_field_max(f: TNcbForm; i: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_max';
function ncb_form_field_noptions(f: TNcbForm; i: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_noptions';
function ncb_form_field_option(f: TNcbForm; i: cint; o: cint): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_form_field_option';

{ ---- Raw externals: answering a form ---- }

function ncb_values_new(n: cint): TNcbValues; cdecl; external NETCHAN_C_LIB name 'ncb_values_new';
procedure ncb_values_free(v: TNcbValues); cdecl; external NETCHAN_C_LIB name 'ncb_values_free';
procedure ncb_values_set_text(v: TNcbValues; i: cint; text: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_values_set_text';
procedure ncb_values_set_number(v: TNcbValues; i: cint; number: cint32); cdecl; external NETCHAN_C_LIB name 'ncb_values_set_number';
procedure ncb_values_set_choice(v: TNcbValues; i: cint; choice: cuint32); cdecl; external NETCHAN_C_LIB name 'ncb_values_set_choice';
procedure ncb_values_set_bool(v: TNcbValues; i: cint; on_: cint); cdecl; external NETCHAN_C_LIB name 'ncb_values_set_bool';
procedure ncb_auth_submit(a: TNcbAuth; v: TNcbValues); cdecl; external NETCHAN_C_LIB name 'ncb_auth_submit';

{ ---- Raw externals: reading a submission ---- }

function ncb_auth_value_text(a: TNcbAuth; name: PAnsiChar): PAnsiChar; cdecl; external NETCHAN_C_LIB name 'ncb_auth_value_text';
function ncb_auth_value_int(a: TNcbAuth; name: PAnsiChar; fallback: cint32): cint32; cdecl; external NETCHAN_C_LIB name 'ncb_auth_value_int';
function ncb_auth_value_bool(a: TNcbAuth; name: PAnsiChar; fallback: cint): cint; cdecl; external NETCHAN_C_LIB name 'ncb_auth_value_bool';
function ncb_auth_value_choice(a: TNcbAuth; name: PAnsiChar): cuint32; cdecl; external NETCHAN_C_LIB name 'ncb_auth_value_choice';

procedure ncb_auth_server_token(a: TNcbAuth; tok: PByte; len: csize_t); cdecl; external NETCHAN_C_LIB name 'ncb_auth_server_token';

{ ---- Raw externals: building a server form ---- }

procedure ncb_form_builder_title(fb: TNcbFormBuilder; title: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_title';
procedure ncb_form_builder_note(fb: TNcbFormBuilder; note: PAnsiChar); cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_note';
function ncb_form_builder_add_field(fb: TNcbFormBuilder; type_: cint; flags: cint; name: PAnsiChar; label_: PAnsiChar; value: PAnsiChar; pattern: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_add_field';
procedure ncb_form_builder_field_range(fb: TNcbFormBuilder; field: cint; min_: cint32; max_: cint32); cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_field_range';
function ncb_form_builder_field_option(fb: TNcbFormBuilder; field: cint; option: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_field_option';
function ncb_form_builder_add_error(fb: TNcbFormBuilder; name: PAnsiChar; message: PAnsiChar): cint; cdecl; external NETCHAN_C_LIB name 'ncb_form_builder_add_error';

{ ---- Raw externals: framing ---- }

function ncb_auth_frame(out_: Pointer; outlen: csize_t; msg: Pointer; len: csize_t): clong; cdecl; external NETCHAN_C_LIB name 'ncb_auth_frame';
function ncb_framer_new(cap: csize_t): TNcbFramer; cdecl; external NETCHAN_C_LIB name 'ncb_framer_new';
procedure ncb_framer_free(f: TNcbFramer); cdecl; external NETCHAN_C_LIB name 'ncb_framer_free';
function ncb_framer_push(f: TNcbFramer; bytes: Pointer; n: csize_t): cint; cdecl; external NETCHAN_C_LIB name 'ncb_framer_push';
function ncb_framer_next(f: TNcbFramer; out_: Pointer; cap: csize_t): clong; cdecl; external NETCHAN_C_LIB name 'ncb_framer_next';

{ ---- Helpers: addresses ---- }

{ Fill a from a host name or literal address. Returns NCB_OK or NCB_ERR. }
function NcbAddrFrom(a: TNcbAddr; const host: string; port: cuint16): cint;

{ ---- Helpers: packet I/O ---- }

{ Feed one datagram. An empty pkt is passed as a zero-length buffer. }
function NcbFeedBytes(c: TNcbConn; const pkt: RawByteString; from: TNcbAddr; nowMs: cuint32): cint;

{ Next outgoing datagram, or an empty string when nothing is pending.
  dest receives the peer address. }
function NcbSendNext(c: TNcbConn; dest: TNcbAddr; nowMs: cuint32): RawByteString;

{ ---- Helpers: channels ---- }

{ Queue data on a channel. Returns the shim count (bytes queued or a negative code). }
function NcbChanWrite(ch: TNcbChan; const data: RawByteString): cint;

{ Read one datagram or stream chunk. Returns an empty string when nothing is
  queued. An empty datagram also reads back as an empty string. }
function NcbChanRead(ch: TNcbChan): RawByteString;

{ ---- Helpers: crypto ---- }

{ Next handshake packet for this side, or an empty string when none. }
function NcbCryptoHandshake(c: TNcbCrypto): RawByteString;

{ Seal plain into sealed. Returns the shim length, or NC_CRYPTO_ERR. }
function NcbCryptoSeal(c: TNcbCrypto; const plain: RawByteString; var sealed: RawByteString): clong;

{ Open pkt into plain. Returns the shim length (0 means nothing to deliver,
  such as a handshake packet), or NC_CRYPTO_ERR. }
function NcbCryptoOpen(c: TNcbCrypto; const pkt: RawByteString; var plain: RawByteString): clong;

{ ---- Helpers: beacon ---- }

{ Build the beacon into pkt. Returns the shim length, or NC_BEACON_ERR. }
function NcbBeaconBuild(b: TNcbBeacon; var pkt: RawByteString): clong;

{ ---- Helpers: framing ---- }

{ Frame one message with its 2-byte length prefix. Empty string on error. }
function NcbFrame(const msg: RawByteString): RawByteString;

{ Take the next whole framed message into msg. Returns its length, 0 when
  more bytes are needed, or NC_AUTH_ERR. }
function NcbFramerNext(f: TNcbFramer; var msg: RawByteString): clong;

implementation

const
  NcbSendBufSize = 2048;
  NcbFrameMax    = 65535;

function NcbAddrFrom(a: TNcbAddr; const host: string; port: cuint16): cint;
begin
  NcbAddrFrom := ncb_addr_from(a, PAnsiChar(host), port);
end;

function NcbFeedBytes(c: TNcbConn; const pkt: RawByteString; from: TNcbAddr; nowMs: cuint32): cint;
begin
  if pkt = '' then
    NcbFeedBytes := ncb_feed(c, nil, 0, from, nowMs)
  else
    NcbFeedBytes := ncb_feed(c, @pkt[1], csize_t(Length(pkt)), from, nowMs);
end;

function NcbSendNext(c: TNcbConn; dest: TNcbAddr; nowMs: cuint32): RawByteString;
var
  buf: array of cuint8;
  n: csize_t;
begin
  SetLength(buf, NcbSendBufSize);
  n := ncb_send_next(c, @buf[0], csize_t(NcbSendBufSize), dest, nowMs);
  SetLength(NcbSendNext, n);
  if n > 0 then
    Move(buf[0], NcbSendNext[1], n);
end;

function NcbChanWrite(ch: TNcbChan; const data: RawByteString): cint;
begin
  if data = '' then
    NcbChanWrite := ncb_chan_write(ch, nil, 0)
  else
    NcbChanWrite := ncb_chan_write(ch, @data[1], csize_t(Length(data)));
end;

function NcbChanRead(ch: TNcbChan): RawByteString;
var
  buf: array of cuint8;
  n: cint;
begin
  SetLength(buf, NcbSendBufSize);
  n := ncb_chan_read(ch, @buf[0], csize_t(NcbSendBufSize));
  if n < 0 then
    n := 0;
  SetLength(NcbChanRead, n);
  if n > 0 then
    Move(buf[0], NcbChanRead[1], n);
end;

function NcbCryptoHandshake(c: TNcbCrypto): RawByteString;
var
  buf: array of cuint8;
  n: csize_t;
begin
  SetLength(buf, 256);
  n := ncb_crypto_handshake_packet(c, @buf[0], csize_t(256));
  SetLength(NcbCryptoHandshake, n);
  if n > 0 then
    Move(buf[0], NcbCryptoHandshake[1], n);
end;

function NcbCryptoSeal(c: TNcbCrypto; const plain: RawByteString; var sealed: RawByteString): clong;
var
  buf: array of cuint8;
  cap: csize_t;
  n: clong;
begin
  cap := csize_t(Length(plain) + NC_CRYPTO_OVERHEAD);
  SetLength(buf, cap);
  if Length(plain) = 0 then
    n := ncb_crypto_seal(c, nil, 0, @buf[0], cap)
  else
    n := ncb_crypto_seal(c, @plain[1], csize_t(Length(plain)), @buf[0], cap);
  if n < 0 then
    SetLength(sealed, 0)
  else begin
    SetLength(sealed, n);
    if n > 0 then
      Move(buf[0], sealed[1], n);
  end;
  NcbCryptoSeal := n;
end;

function NcbCryptoOpen(c: TNcbCrypto; const pkt: RawByteString; var plain: RawByteString): clong;
var
  buf: array of cuint8;
  cap: csize_t;
  n: clong;
begin
  cap := csize_t(Length(pkt) + 1);
  SetLength(buf, cap);
  if Length(pkt) = 0 then
    n := ncb_crypto_open(c, nil, 0, @buf[0], cap)
  else
    n := ncb_crypto_open(c, @pkt[1], csize_t(Length(pkt)), @buf[0], cap);
  if n < 0 then
    SetLength(plain, 0)
  else begin
    SetLength(plain, n);
    if n > 0 then
      Move(buf[0], plain[1], n);
  end;
  NcbCryptoOpen := n;
end;

function NcbBeaconBuild(b: TNcbBeacon; var pkt: RawByteString): clong;
var
  buf: array of cuint8;
  n: clong;
begin
  SetLength(buf, NC_BEACON_MAX);
  n := ncb_beacon_build(@buf[0], csize_t(NC_BEACON_MAX), b);
  if n < 0 then
    SetLength(pkt, 0)
  else begin
    SetLength(pkt, n);
    if n > 0 then
      Move(buf[0], pkt[1], n);
  end;
  NcbBeaconBuild := n;
end;

function NcbFrame(const msg: RawByteString): RawByteString;
var
  buf: array of cuint8;
  n: clong;
begin
  SetLength(NcbFrame, 0);
  if (Length(msg) = 0) or (Length(msg) > NcbFrameMax) then
    exit;
  SetLength(buf, Length(msg) + 2);
  n := ncb_auth_frame(@buf[0], csize_t(Length(msg) + 2), @msg[1], csize_t(Length(msg)));
  if n <= 0 then
    exit;
  SetLength(NcbFrame, n);
  Move(buf[0], NcbFrame[1], n);
end;

function NcbFramerNext(f: TNcbFramer; var msg: RawByteString): clong;
var
  buf: array of cuint8;
  n: clong;
begin
  SetLength(buf, NcbFrameMax);
  n := ncb_framer_next(f, @buf[0], csize_t(NcbFrameMax));
  if n <= 0 then
    SetLength(msg, 0)
  else begin
    SetLength(msg, n);
    Move(buf[0], msg[1], n);
  end;
  NcbFramerNext := n;
end;

end.
