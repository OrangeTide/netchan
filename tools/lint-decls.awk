# lint-decls.awk : find declarations placed after the first statement of a block
#
# Invoked once per file by tools/lint.sh. The style asks for declarations at
# the top of the block that uses them, so this reports any that sit below a
# statement. Parse-check it with `awk -f tools/lint-decls.awk /dev/null` and
# lint it with `gawk --lint`.
#
# What it deliberately does not report:
#   - struct, union and enum bodies, which are member lists, not statements
#   - the continuation lines of a declaration whose initializer wraps
#   - anything after a preprocessor line, since the two arms of an #if are
#     not one sequence

function cnt(s, ch,   i, n) {
    n = 0
    for (i = 1; i <= length(s); i++)
        if (substr(s, i, 1) == ch) n++
    return n
}

FNR == 1 { depth = 0; delete seen; inagg = 0; pending = 0 }

{
    line = $0
    sub(/\/\*.*\*\//, "", line)
    sub(/\/\/.*$/, "", line)

    if (line ~ /^[ \t]*$/) next
    if (line ~ /^[ \t]*(\*|\/\*)/) next
    if (line ~ /^[ \t]*#/) { seen[depth] = 0; next }

    nopen = cnt(line, "{")
    nclose = cnt(line, "}")

    isctl = (line ~ /^[ \t]*(if|for|while|switch|return|else|do|case|default|goto|break|continue)[^a-zA-Z_0-9]/)

    # A declaration is "<type> <name>;" with no parentheses, or
    # "<type> <name> = <expr>;" where the text left of the = has none. That
    # is what separates it from an assignment or a call.
    isdecl = 0
    decl_head = "^[ \t]+(static +)?(const +)?(volatile +)?((struct|union|enum) +)?([a-zA-Z_][a-zA-Z_0-9]* +){1,3}\\**[a-zA-Z_][a-zA-Z_0-9]*[ \t]*(\\[[^]]*\\])?[ \t]*"
    if (!isctl) {
        if (line !~ /\(/ && line ~ decl_head "(;|,)")
            isdecl = 1
        else if (index(line, "=") > 0) {
            lhs = substr(line, 1, index(line, "=") - 1)
            if (lhs !~ /\(/ && lhs !~ /[-+*\/%&|^!<>]$/ && lhs ~ decl_head "$")
                isdecl = 1
        }
    }

    iscont = pending
    if (pending && line ~ /;[ \t]*$/) pending = 0
    else if (!pending && isdecl && line !~ /;[ \t]*$/) pending = 1

    if (isdecl && !iscont && depth > 0 && seen[depth] && !inagg) {
        printf "%s:%d: declaration after a statement: %s\n",
               FILENAME, FNR, line
        rc = 1
    }

    if (!isdecl && !iscont && depth > 0 &&
        (line ~ /;[ \t]*$/ || line ~ /^[ \t]*(if|for|while|switch|do)[^a-zA-Z_0-9]/))
        seen[depth] = 1

    if (nopen >= 1 && nclose >= 1) seen[depth] = 0
    if (nopen > nclose) {
        if (line ~ /(struct|union|enum)[^;]*\{/) { inagg = 1; aggdepth = depth }
        depth += nopen - nclose
        seen[depth] = 0
    } else if (nclose > nopen) {
        for (d = 0; d < nclose - nopen; d++) { seen[depth] = 0; depth-- }
        if (inagg && depth <= aggdepth) inagg = 0
    }
}

END { exit rc }
