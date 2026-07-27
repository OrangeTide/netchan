---
title: Message encoding
weight: 8
abstract: The IDL, the tagged wire format, and how a reader skips a field it has never heard of.
category: manual
---

A channel moves opaque bytes. What those bytes mean is the application's choice, and `idl/` is one answer: microser, a tagged wire format with a small IDL compiler.

A channel's content type is a topic, in the sense MQTT uses the word. It is set when the channel opens and read back from the peer's `CHANNEL_OPEN`, so a program routes a channel by its topic before it reads a byte of payload. A topic can carry one message type, or many.

Describe a message in an `.idl` file and the compiler emits a C struct and an encode and decode function for it. A field is a type, a name, and a number from 1 to 31.

    message PlayerInput
        uint32 seq   = 1
        uint8  input = 2
    end

The wire format is tagged, so a reader steps over a field it does not know: a newer sender may add fields and an older reader still parses the ones it understands. A `bytes` or `string` field decodes zero-copy, as a pointer into the caller's buffer and a length, so a large payload does not enlarge the struct. Keeping message structs small matters, because a message that exceeds the path limit is fragmented, and fragmentation is the first thing to cost you throughput.

When several message types share one topic, a `dispatch` block multiplexes them on a leading tag byte. Each datagram becomes `<tag><body>`.

    dispatch Chat
        1 Join
        2 Say
        3 Leave
    end

From that block the compiler generates a tag constant per message, an encode helper that prefixes the tag, and a decode entry point that reads the tag into a tagged union. The receiver reads a datagram, decodes, and switches on the type, which mirrors how it already polls `netchan_event`.

    struct chat_msg m;
    chat_decode(buf, len, &m);
    switch (m.type) {
    case CHAT_JOIN:  /* m.u.join  */  break;
    case CHAT_SAY:   /* m.u.say   */  break;
    case CHAT_LEAVE: /* m.u.leave */  break;
    default:         /* CHAT_NONE: a type this build predates; skipped */ break;
    }

An unknown tag is not an error. The body carries its own length, so the decoder steps over a message type it does not recognize and reports the type as `CHAT_NONE`, the same forward compatibility the field level gives you. Tag numbers can follow a hierarchy, for instance one range for control and another for data, since a tag is a full byte. `examples/typed/` shows the whole path end to end. Generation runs at build time and needs a POSIX `awk`; a project that would rather not run it can copy `idl/microser.h` and write the structs by hand.

