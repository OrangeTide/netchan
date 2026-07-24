#!/bin/sh
# gen-api.sh : turn a netchan header into the manual's API reference
# PUBLIC DOMAIN (CC0-1.0)
#
# Usage: gen-api.sh path/to/netchan.h > api.html
#
# Emits an <h2> for each section banner in the header and an entry for each
# exported function, with the comment block above it as its documentation. The
# header is the single source of truth, so the reference cannot drift from the
# code. check-symbols.sh guards the other direction, the prose naming a symbol
# the header no longer declares.
#
# A section banner with no functions under it is dropped, so the reference
# lists only the parts of the header that declare something.

set -eu

HDR=${1:?usage: gen-api.sh path/to/netchan.h}

awk '
# HTML-escape the three characters that matter in element content.
function esc(s) {
    gsub(/&/, "\\&amp;", s)
    gsub(/</, "\\&lt;", s)
    gsub(/>/, "\\&gt;", s)
    return s
}

# A banner is held back until an item follows it, so empty sections vanish.
function flush_banner() {
    if (pending_banner != "") {
        printf "<h2>%s</h2>\n", esc(pending_banner)
        pending_banner = ""
    }
}

function emit_item(sig, name, doc,    d) {
    flush_banner()
    printf "<section class=\"api-item\">\n"
    printf "<h3 id=\"%s\"><code>%s</code></h3>\n", name, esc(name)
    printf "<pre class=\"api-sig\">%s</pre>\n", esc(sig)
    d = doc
    gsub(/^[ \n]+/, "", d)
    gsub(/[ \n]+$/, "", d)
    if (d != "")
        printf "<p>%s</p>\n", esc(d)
    printf "</section>\n"
}

# Strip a leading comment margin: any run of "*" and one space.
function demargin(s) {
    sub(/^[ \t]*\*+[ \t]?/, "", s)
    return s
}

BEGIN { in_comment = 0; is_banner = 0; doc = ""; sig = ""; pending_banner = "" }

# --- inside a comment block ---
in_comment {
    line = $0
    if (line ~ /\*\//) {                 # comment ends here
        sub(/\*\/.*/, "", line)
        in_comment = 0
    }
    line = demargin(line)
    if (is_banner) {
        gsub(/^[ \t]+|[ \t]+$/, "", line)
        if (line != "")
            banner = (banner == "" ? line : banner " " line)
    } else if (line != "" || body != "") {
        body = (body == "" ? line : body "\n" line)
    }
    if (!in_comment) {
        if (is_banner)
            pending_banner = banner
        else
            doc = body
    }
    next
}

# --- a comment block begins ---
/\/\*/ {
    is_banner = ($0 ~ /\/\*\*\*\*/)
    banner = ""; body = ""
    rest = $0
    sub(/.*\/\*/, "", rest)
    in_comment = 1
    if (rest ~ /\*\//) {                 # single-line comment
        sub(/\*\/.*/, "", rest)
        in_comment = 0
    }
    rest = demargin(rest)
    gsub(/^[ \t]+|[ \t]+$/, "", rest)
    if (rest != "") {
        if (is_banner)
            banner = rest
        else
            body = rest
    }
    if (!in_comment) {
        if (is_banner)
            pending_banner = banner
        else
            doc = body
    }
    next
}

# --- accumulating a function declaration across lines ---
sig != "" {
    sig = sig " " $0
    if ($0 ~ /;/) {
        sub(/;.*/, "", sig)
        gsub(/[ \t]+/, " ", sig)
        gsub(/ *\( */, "(", sig)
        emit_item(sig, signame, doc)
        sig = ""; doc = ""
    }
    next
}

# --- a function declaration begins at column 0 ---
/^[A-Za-z].*[a-z0-9_]+ *\(/ && /(netchan|nc)_[a-z0-9_]+ *\(/ && $0 !~ /\{/ {
    signame = $0
    sub(/ *\(.*/, "", signame)
    sub(/.*[^A-Za-z0-9_]/, "", signame)
    sig = $0
    if ($0 ~ /;/) {
        sub(/;.*/, "", sig)
        gsub(/[ \t]+/, " ", sig)
        gsub(/ *\( */, "(", sig)
        emit_item(sig, signame, doc)
        sig = ""; doc = ""
    }
    next
}

# any other non-blank line ends a pending doc (e.g. a struct definition)
/[^ \t]/ { if (sig == "") doc = "" }
' "$HDR"
