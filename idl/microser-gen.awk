# microser-gen.awk : the microser IDL compiler proper
#
# Invoked as `awk -v base=<output> -f microser-gen.awk <input.idl>` by
# microser-gen.sh. Kept as a standalone -f file rather than embedded in a
# shell single-quoted string, so apostrophes and quotes in comments and code
# are ordinary characters, not shell-quote hazards. Parse-check it with
#   awk -f microser-gen.awk /dev/null
# and lint it with `gawk --lint`.

# Range comparisons like (sc >= "A" && sc <= "Z") are locale dependent. Some
# locales collate as aAbBcC..., which puts every lowercase letter inside that
# range and mangles "event" into "e_v_e_n_t". index() against an explicit set
# is collation independent, so use that instead.
function to_snake(s,    r, si, sc) {
    r = ""
    for (si = 1; si <= length(s); si++) {
        sc = substr(s, si, 1)
        if (index("ABCDEFGHIJKLMNOPQRSTUVWXYZ", sc) > 0) {
            if (si > 1) r = r "_"
            r = r tolower(sc)
        } else {
            r = r sc
        }
    }
    return r
}

function is_bytes(ty) {
    return (ty == "bytes" || ty == "string")
}

function is_enum(ty,    ei) {
    for (ei = 1; ei <= ne; ei++)
        if (ty == en[ei]) return 1
    return 0
}

function c_type(ty) {
    if (ty ~ /^u?int(8|16|32|64)$/) return ty "_t"
    if (is_bytes(ty)) return ""
    return to_snake(ty) "_t"
}

# The read/write helper suffix for a type. An enum is a uint8, which is what
# the typedef the header emits for it says too.
function rw_suffix(ty) {
    if (ty == "uint8")  return "u8"
    if (ty == "int8")   return "i8"
    if (ty == "uint16") return "u16"
    if (ty == "int16")  return "i16"
    if (ty == "uint32") return "u32"
    if (ty == "int32")  return "i32"
    if (ty == "uint64") return "u64"
    if (ty == "int64")  return "i64"
    if (is_enum(ty))    return "u8"
    return ""
}

function fail(msg) {
    printf "%s: %s\n", FILENAME, msg > "/dev/stderr"
    errors++
}

# Declare the running counts so every later reference is to a variable this
# program has set. The base check lives in END, not here: an exit in BEGIN
# still runs END, which would generate empty files, so the guard has to be
# where a failed check can stop before any output.
BEGIN {
    nm = 0; ne = 0; nd = 0; errors = 0
    state = ""
}

# strip comments and whitespace
{
    sub(/#.*/, "")
    gsub(/^[ \t]+|[ \t]+$/, "")
    if ($0 == "") next
}

# --- enum ---
$1 == "enum" {
    ne++
    en[ne] = $2
    enc[ne] = 0
    state = "enum"
    next
}
state == "enum" && $1 == "end" { state = ""; next }
state == "enum" {
    split($0, p, /[ \t]*=[ \t]*/)
    enc[ne]++
    j = enc[ne]
    ee_name[ne, j] = p[1]
    ee_val[ne, j] = p[2] + 0
    enum_lookup[p[1]] = p[2] + 0
    next
}

# --- message ---
$1 == "message" {
    nm++
    mn[nm] = $2
    mrf[nm] = 0
    maf[nm] = 0
    mcase[nm] = 0
    state = "msg"
    next
}
state == "msg" && $1 == "end" { state = ""; next }

# case block start
state == "msg" && $1 == "case" {
    mcase[nm] = 1
    mdt[nm] = $2; mdn[nm] = $3; mdtag[nm] = $5 + 0
    mnv[nm] = 0
    # add discriminant to all-fields list
    maf[nm]++; j = maf[nm]
    af_type[nm, j] = $2; af_name[nm, j] = $3; af_tag[nm, j] = $5 + 0
    state = "case"
    next
}

# regular field
state == "msg" {
    mrf[nm]++; j = mrf[nm]
    rf_type[nm, j] = $1; rf_name[nm, j] = $2; rf_tag[nm, j] = $4 + 0
    maf[nm]++; k = maf[nm]
    af_type[nm, k] = $1; af_name[nm, k] = $2; af_tag[nm, k] = $4 + 0
    next
}

# --- case variants ---
state == "case" && $1 == "end" { state = "msg"; next }

# variant label
(state == "case" || state == "var") && $0 ~ /:$/ {
    label = $1; sub(/:$/, "", label)
    mnv[nm]++; vi = mnv[nm]
    # A bare number is a legitimate label. A name is only legitimate if some
    # enum declared it: awk would otherwise turn the typo into 0, and several
    # such labels in one message would collide on "case 0:" in the generated
    # C. Record the failure and let END report it against the .idl.
    if (label in enum_lookup)
        vv[nm, vi] = enum_lookup[label]
    else if (label ~ /^-?[0-9]+$/)
        vv[nm, vi] = label + 0
    else {
        vv[nm, vi] = 0
        vunknown[nm, vi] = 1
    }
    vlabel[nm, vi] = label
    vnf[nm, vi] = 0
    state = "var"
    next
}

state == "var" && $1 == "end" { state = "msg"; next }

# variant field
#
# Every variant field becomes its own struct member and its own decode case.
# The struct is flat, not a union of per-variant structs, so two variants
# cannot share a field number: the shared member would take one name and type,
# and a second variant would encode a member that does not exist. The
# tag-uniqueness check in END enforces that. A message that sets only the
# fields of the active variant still decodes cleanly, since the rest stay zero.
# (No apostrophes here: this awk program lives inside shell single quotes.)
state == "var" {
    vnf[nm, vi]++; vfi = vnf[nm, vi]
    vf_type[nm, vi, vfi] = $1
    vf_name[nm, vi, vfi] = $2
    vf_tag[nm, vi, vfi] = $4 + 0
    maf[nm]++; k = maf[nm]
    af_type[nm, k] = $1; af_name[nm, k] = $2; af_tag[nm, k] = $4 + 0
    next
}

# --- dispatch: a set of messages multiplexed on a leading tag byte ---
# Each line inside the block is "<tag> <MessageName>". The generator emits a
# tagged-union struct, a per-message encode helper that prefixes the tag, and
# a decode entry point that reads the tag and fills the union.
$1 == "dispatch" {
    nd++
    dn[nd] = $2
    ndm[nd] = 0
    state = "dispatch"
    next
}
state == "dispatch" && $1 == "end" { state = ""; next }
state == "dispatch" {
    ndm[nd]++
    k = ndm[nd]
    dm_tag[nd, k] = $1 + 0
    dm_msg[nd, k] = $2
    next
}

# --- code generation ---
END {
    # base arrives through -v and names the output files. Without it there is
    # nowhere to write, so stop before generating anything. An exit here is
    # terminal, unlike one in BEGIN, which would still fall through to this
    # block and emit empty files.
    if (base == "") {
        print "microser-gen.awk: no output base (pass -v base=...)" > "/dev/stderr"
        close("/dev/stderr")
        exit 1
    }

    # Check before writing anything, so a rejected .idl leaves no half
    # generated files behind for the build to pick up.
    for (i = 1; i <= nm; i++) {
        for (j = 1; j <= maf[i]; j++) {
            t = af_type[i, j]
            if (!is_bytes(t) && rw_suffix(t) == "")
                fail(sprintf("message %s: field \"%s\" has unknown type \"%s\"",
                     mn[i], af_name[i, j], t))
            if (af_tag[i, j] < 1 || af_tag[i, j] > 31)
                fail(sprintf("message %s: field \"%s\" has number %d, " \
                     "outside 1 to 31", mn[i], af_name[i, j], af_tag[i, j]))
        }
        # af[] holds every field of the message: the plain fields, the
        # discriminant, and every variant field. It is exactly what becomes
        # the struct members and the decode switch, so every field number must
        # be unique across the whole message, variants included. A duplicate
        # would be a repeated struct member or a duplicate case label in the
        # generated C. Reject it here with a clean message.
        for (j = 1; j <= maf[i]; j++) {
            for (k = j + 1; k <= maf[i]; k++) {
                if (af_tag[i, j] == af_tag[i, k])
                    fail(sprintf("message %s: \"%s\" and \"%s\" share field " \
                         "number %d", mn[i], af_name[i, j], af_name[i, k],
                         af_tag[i, j]))
            }
        }
        # The variant labels become the case labels of the discriminant
        # switch, so they answer to the same rule the field numbers do: each
        # one has to mean something, and no two may mean the same thing.
        for (j = 1; j <= mnv[i]; j++) {
            if ((i, j) in vunknown)
                fail(sprintf("message %s: variant label \"%s\" is neither a " \
                     "number nor a value of any declared enum",
                     mn[i], vlabel[i, j]))
            for (k = j + 1; k <= mnv[i]; k++) {
                if (!((i, j) in vunknown) && !((i, k) in vunknown) &&
                    vv[i, j] == vv[i, k])
                    fail(sprintf("message %s: variants \"%s\" and \"%s\" are " \
                         "both %d", mn[i], vlabel[i, j], vlabel[i, k],
                         vv[i, j]))
            }
        }
    }
    for (di = 1; di <= nd; di++) {
        for (dk = 1; dk <= ndm[di]; dk++) {
            found = 0
            for (mi = 1; mi <= nm; mi++)
                if (mn[mi] == dm_msg[di, dk]) found = 1
            if (!found)
                fail(sprintf("dispatch %s: no message named \"%s\"",
                     dn[di], dm_msg[di, dk]))
            # Tag 0 is reserved for an unrecognised message, and a tag is one
            # byte on the wire.
            if (dm_tag[di, dk] < 1 || dm_tag[di, dk] > 255)
                fail(sprintf("dispatch %s: \"%s\" has tag %d, outside 1 to 255",
                     dn[di], dm_msg[di, dk], dm_tag[di, dk]))
            for (dk2 = dk + 1; dk2 <= ndm[di]; dk2++)
                if (dm_tag[di, dk] == dm_tag[di, dk2])
                    fail(sprintf("dispatch %s: \"%s\" and \"%s\" share tag %d",
                         dn[di], dm_msg[di, dk], dm_msg[di, dk2],
                         dm_tag[di, dk]))
        }
    }
    if (errors > 0) {
        close("/dev/stderr")
        exit 1
    }

    h = base ".h"
    c = base ".c"
    guard = toupper(base) "_H"
    gsub(/[^A-Z0-9_]/, "_", guard)

    # The .c includes its header by bare name, not by the path it was
    # written to.
    hbase = base
    sub(/.*\//, "", hbase)

    # ---- header ----
    printf "/* Generated from %s - do not edit */\n\n", FILENAME > h
    printf "#ifndef %s\n#define %s\n\n#include \"microser.h\"\n\n", guard, guard > h

    for (i = 1; i <= ne; i++) {
        sn = to_snake(en[i]); un = toupper(sn)
        printf "typedef uint8_t %s_t;\n", sn > h
        for (j = 1; j <= enc[i]; j++) {
            ename = toupper(to_snake(ee_name[i, j]))
            printf "#define %s_%s %d\n", un, ename, ee_val[i, j] > h
        }
        printf "\n" > h
    }

    for (i = 1; i <= nm; i++) {
        sn = to_snake(mn[i])
        printf "struct %s {\n", sn > h
        for (j = 1; j <= maf[i]; j++) {
            if (af_type[i, j] == "bytes") {
                printf "    const uint8_t *%s;\n", af_name[i, j] > h
                printf "    uint16_t %s_len;\n", af_name[i, j] > h
            } else if (af_type[i, j] == "string") {
                printf "    const char *%s;\n", af_name[i, j] > h
                printf "    uint16_t %s_len;\n", af_name[i, j] > h
            } else {
                printf "    %s %s;\n", c_type(af_type[i, j]), af_name[i, j] > h
            }
        }
        printf "};\n\n" > h
        printf "int %s_encode(const struct %s *msg, uint8_t *buf, int len);\n", sn, sn > h
        printf "int %s_decode(struct %s *msg, const uint8_t *buf, int len);\n\n", sn, sn > h
    }

    for (i = 1; i <= nd; i++) {
        dsn = to_snake(dn[i]); dun = toupper(dsn)
        printf "/* dispatch %s: <tag byte><message body> on one channel. */\n", dn[i] > h
        printf "#define %s_NONE 0\n", dun > h
        for (k = 1; k <= ndm[i]; k++) {
            msn = to_snake(dm_msg[i, k])
            printf "#define %s_%s %d\n", dun, toupper(msn), dm_tag[i, k] > h
        }
        printf "\n" > h
        printf "struct %s_msg {\n", dsn > h
        printf "    int type;    /* one of the %s_* tags, or %s_NONE */\n", dun, dun > h
        printf "    union {\n" > h
        for (k = 1; k <= ndm[i]; k++) {
            msn = to_snake(dm_msg[i, k])
            printf "        struct %s %s;\n", msn, msn > h
        }
        printf "    } u;\n" > h
        printf "};\n\n" > h
        for (k = 1; k <= ndm[i]; k++) {
            msn = to_snake(dm_msg[i, k])
            printf "int %s_encode_%s(uint8_t *buf, int len, const struct %s *msg);\n", dsn, msn, msn > h
        }
        printf "int %s_msg_type(const uint8_t *buf, int len);\n", dsn > h
        printf "int %s_decode(const uint8_t *buf, int len, struct %s_msg *out);\n\n", dsn, dsn > h
    }

    printf "#endif /* %s */\n", guard > h
    close(h)

    # ---- source ----
    printf "/* Generated from %s - do not edit */\n\n", FILENAME > c
    printf "#include \"%s\"\n", hbase ".h" > c
    printf "#include <string.h>\n\n" > c

    for (i = 1; i <= nm; i++) {
        sn = to_snake(mn[i])

        # encode
        printf "int %s_encode(const struct %s *msg, uint8_t *buf, int len)\n", sn, sn > c
        printf "{\n    int pos = 2;\n\n" > c

        for (j = 1; j <= mrf[i]; j++) {
            if (is_bytes(rf_type[i, j])) {
                printf "    pos = ms_write_tag_bytes(buf, pos, len, %d,\n", rf_tag[i, j] > c
                printf "        (const void *)msg->%s, msg->%s_len);\n", rf_name[i, j], rf_name[i, j] > c
            } else {
                printf "    pos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                    rw_suffix(rf_type[i, j]), rf_tag[i, j], rf_name[i, j] > c
            }
            printf "    if (pos < 0) return MS_ERR;\n" > c
        }

        if (mcase[i]) {
            printf "    pos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                rw_suffix(mdt[i]), mdtag[i], mdn[i] > c
            printf "    if (pos < 0) return MS_ERR;\n" > c
            printf "    switch (msg->%s) {\n", mdn[i] > c
            for (vi = 1; vi <= mnv[i]; vi++) {
                printf "    case %d: /* %s */\n", vv[i, vi], vlabel[i, vi] > c
                for (vfi = 1; vfi <= vnf[i, vi]; vfi++) {
                    if (is_bytes(vf_type[i, vi, vfi])) {
                        printf "        pos = ms_write_tag_bytes(buf, pos, len, %d,\n", vf_tag[i, vi, vfi] > c
                        printf "            (const void *)msg->%s, msg->%s_len);\n", vf_name[i, vi, vfi], vf_name[i, vi, vfi] > c
                    } else {
                        printf "        pos = ms_write_tag_%s(buf, pos, len, %d, msg->%s);\n", \
                            rw_suffix(vf_type[i, vi, vfi]), vf_tag[i, vi, vfi], vf_name[i, vi, vfi] > c
                    }
                    printf "        if (pos < 0) return MS_ERR;\n" > c
                }
                printf "        break;\n" > c
            }
            printf "    }\n" > c
        }

        printf "\n    buf[0] = (uint8_t)((pos - 2) & 0xff);\n" > c
        printf "    buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);\n" > c
        printf "    return pos;\n}\n\n" > c

        # decode
        printf "int %s_decode(struct %s *msg, const uint8_t *buf, int len)\n", sn, sn > c
        printf "{\n    int end, pos = 2;\n\n" > c
        printf "    if (len < 2) return MS_ERR;\n" > c
        printf "    end = (int)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8)) + 2;\n" > c
        printf "    if (end > len) return MS_ERR;\n" > c
        printf "    memset(msg, 0, sizeof(*msg));\n\n" > c
        printf "    while (pos < end) {\n" > c
        printf "        uint8_t tag = buf[pos++];\n" > c
        printf "        switch (tag >> 3) {\n" > c

        for (j = 1; j <= maf[i]; j++) {
            printf "        case %d:\n", af_tag[i, j] > c
            if (af_type[i, j] == "string") {
                printf "            {\n" > c
                printf "                const uint8_t *_tmp = 0;\n" > c
                printf "                pos = ms_read_bytes(buf, pos, end,\n" > c
                printf "                    &_tmp, 65535, &msg->%s_len);\n", af_name[i, j] > c
                printf "                msg->%s = (const char *)_tmp;\n", af_name[i, j] > c
                printf "            }\n" > c
            } else if (af_type[i, j] == "bytes") {
                printf "            pos = ms_read_bytes(buf, pos, end,\n" > c
                printf "                &msg->%s, 65535, &msg->%s_len);\n", af_name[i, j], af_name[i, j] > c
            } else {
                printf "            pos = ms_read_%s(buf, pos, end, &msg->%s);\n", \
                    rw_suffix(af_type[i, j]), af_name[i, j] > c
            }
            printf "            break;\n" > c
        }

        printf "        default:\n" > c
        printf "            pos = ms_skip(buf, pos, end, tag & 7);\n" > c
        printf "            break;\n" > c
        printf "        }\n" > c
        printf "        if (pos < 0) return MS_ERR;\n" > c
        printf "    }\n" > c
        printf "    return end;\n}\n\n" > c
    }

    for (i = 1; i <= nd; i++) {
        dsn = to_snake(dn[i]); dun = toupper(dsn)

        # encode helper per message: tag byte, then the microser body
        for (k = 1; k <= ndm[i]; k++) {
            msn = to_snake(dm_msg[i, k])
            printf "int %s_encode_%s(uint8_t *buf, int len, const struct %s *msg)\n", dsn, msn, msn > c
            printf "{\n    int n;\n\n" > c
            printf "    if (len < 1) return MS_ERR;\n" > c
            printf "    buf[0] = %s_%s;\n", dun, toupper(msn) > c
            printf "    n = %s_encode(msg, buf + 1, len - 1);\n", msn > c
            printf "    if (n < 0) return MS_ERR;\n" > c
            printf "    return n + 1;\n}\n\n" > c
        }

        # the tag alone, for a caller that decodes into its own struct
        printf "int %s_msg_type(const uint8_t *buf, int len)\n", dsn > c
        printf "{\n    if (len < 1) return MS_ERR;\n    return buf[0];\n}\n\n" > c

        # decode: read the tag, fill the union, or skip an unknown message
        printf "int %s_decode(const uint8_t *buf, int len, struct %s_msg *out)\n", dsn, dsn > c
        printf "{\n    int n;\n\n" > c
        printf "    if (len < 1) return MS_ERR;\n" > c
        printf "    out->type = buf[0];\n" > c
        printf "    switch (buf[0]) {\n" > c
        for (k = 1; k <= ndm[i]; k++) {
            msn = to_snake(dm_msg[i, k])
            printf "    case %s_%s:\n", dun, toupper(msn) > c
            printf "        n = %s_decode(&out->u.%s, buf + 1, len - 1);\n", msn, msn > c
            printf "        break;\n" > c
        }
        printf "    default:\n" > c
        printf "        /* An unknown message still carries its own length\n" > c
        printf "         * prefix, so step over it the way microser steps over\n" > c
        printf "         * an unknown field. */\n" > c
        printf "        out->type = %s_NONE;\n", dun > c
        printf "        if (len < 3) return MS_ERR;\n" > c
        printf "        n = (int)((uint16_t)buf[1] | ((uint16_t)buf[2] << 8)) + 2;\n" > c
        printf "        if (n + 1 > len) return MS_ERR;\n" > c
        printf "        break;\n" > c
        printf "    }\n" > c
        printf "    if (n < 0) return MS_ERR;\n" > c
        printf "    return n + 1;\n}\n\n" > c
    }

    close(c)
}
