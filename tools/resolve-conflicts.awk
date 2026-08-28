#!/usr/bin/gawk -f
#
# resolve-conflicts.awk -- walk the conflict blocks in a merged file and, for
# each one, ask on /dev/tty which side to keep.  The resolved file is written to
# stdout; every prompt goes to the tty, so stdout stays clean for redirection.
#
#   gawk -v branch=main -f resolve-conflicts.awk file > file.resolved
#
# Variables:
#   branch   label used for the non-HEAD side in prompts   (default "main")
#   maxshow  max lines shown per side before eliding       (default 40)
#   sticky   preseed an answer applied to every conflict   (h|m|b|n)
#
# Exit status: 0 resolved, 2 aborted by the user, 3 malformed conflict markers.

BEGIN {
    tty = "/dev/tty"
    if (branch == "")  branch = "main"
    if (maxshow == "") maxshow = 40
    conflicts = 0
}

# --- conflict block state machine -------------------------------------------

/^<<<<<<< / {
    if (in_head || in_base || in_main) fail("nested <<<<<<< marker")
    in_head = 1; nh = 0; nm = 0; start = FNR
    next
}

in_head && /^\|\|\|\|\|\|\|/ { in_head = 0; in_base = 1; next }   # diff3 style
in_base && /^=======$/       { in_base = 0; in_main = 1; next }
in_head && /^=======$/       { in_head = 0; in_main = 1; next }

in_main && /^>>>>>>> / {
    in_main = 0
    choose()
    conflicts++
    next
}

in_head { head[++nh] = $0; next }
in_base                     { next }          # ancestor text is never kept
in_main { main[++nm] = $0; next }

{ print }

END {
    if (aborted) exit 2
    if (in_head || in_base || in_main) fail("unterminated conflict block")
    printf "%s: %d conflict%s resolved\n", FILENAME, conflicts, \
           (conflicts == 1 ? "" : "s") > "/dev/stderr"
}

# --- helpers ----------------------------------------------------------------

function fail(msg) {
    printf "resolve-conflicts: %s at %s:%d\n", msg, FILENAME, FNR > "/dev/stderr"
    exit 3
}

function show(title, color, arr, n,   i, lim) {
    printf "\033[%sm--- %s (%d line%s) ---\033[0m\n", \
           color, title, n, (n == 1 ? "" : "s") > tty
    lim = (n > maxshow ? maxshow : n)
    for (i = 1; i <= lim; i++) printf "  %s\n", arr[i] > tty
    if (n > lim) printf "  \033[2m... %d more line%s\033[0m\n", \
                        n - lim, (n - lim == 1 ? "" : "s") > tty
}

function emit(c,   i) {
    if (c == "h" || c == "b") for (i = 1; i <= nh; i++) print head[i]
    if (c == "m" || c == "b") for (i = 1; i <= nm; i++) print main[i]
}

function choose(   ans, c) {
    if (sticky != "") { emit(sticky); return }

    printf "\n\033[1m%s\033[0m  \033[2mlines %d-%d  (#%d)\033[0m\n", \
           FILENAME, start, FNR, conflicts + 1 > tty
    show("HEAD", "36", head, nh)
    show(branch, "33", main, nm)

    while (1) {
        printf "keep [h]ead / [m]%s / [b]oth / [n]either  \033[2m(caps = same for rest of file, q = abort)\033[0m: ", \
               branch > tty
        fflush()   # bare: gawk will not flush a name it also has open for reading
        if ((getline ans < tty) <= 0) {
            printf "\nresolve-conflicts: no input on tty, aborting\n" > "/dev/stderr"
            aborted = 1
            exit 2
        }
        c = substr(ans, 1, 1)
        if (c == "q" || c == "Q") {
            printf "resolve-conflicts: aborted at %s:%d\n", FILENAME, start > "/dev/stderr"
            aborted = 1
            exit 2
        }
        if (c ~ /^[HMBN]$/) { sticky = tolower(c); c = sticky }
        if (c ~ /^[hmbn]$/) { emit(c); return }
        printf "  \033[31m?\033[0m answer h, m, b, n or q\n" > tty
    }
}
