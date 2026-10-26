#!/usr/bin/env bash
#
# resolve-branding.sh [-n] [--keep ours|theirs] [--mode auto|conflict|staged]
#                     [--learn-from REV] [file ...]
#
# Pre-pass for a merge between origin/main and bitedj/main, in either
# direction.  Both directions have the same job: keep the branding of the
# branch being merged *into*, whatever the incoming side says.  Which of the
# two modes below runs is picked from the state of the index, and -h prints
# this text.
#
# conflict mode -- "merge bitedj/main back into origin/main".
#
# The two repos shared no history, so every file that differs came back as an
# add/add conflict, and most of what differs is only the BiteDJ palette repaint
# -- 39 of the 40 conflicts in style.qss are nothing but #f15921 -> #c9372c and
# #488ab3 -> #855ea7.  Those are noise; this drops them so the interactive
# resolve-conflicts.sh only has to ask about the changes that carry meaning.
#
# Two rules, both "keep ours":
#
#   branding assets    listed in BRANDING_PATHS -- the logo and the recoloured
#                      icon.  Binary, never mergeable, always ours.
#
#   colour-managed     listed in COLOR_FILES.  A conflict block is resolved to
#                      ours when the two sides are identical once every colour
#                      literal is blanked out.  Any block with a real change in
#                      it is left alone, markers intact, for a human.
#
# Note on merge.conflictStyle: under diff3/zdiff3 and an empty merge base -- which
# is what an unrelated-history merge gives you -- git stops splitting and hands
# back the whole file as a single conflict block, which no rule can classify.  A
# colour-managed file that comes back that way is re-split from the index stages
# in plain style first, so the result does not depend on the caller's config.
#
# staged mode -- "merge main into bitedj-main", which is a squash merge.
#
# The branches share history now, so that direction does not conflict at all:
# git takes main's side of every branding file and stages it, and the BiteDJ
# repaint is gone from the squash with nothing to review.  There are no index
# stages to work from here, so the comparison is against HEAD (= ours = the
# branch being merged into), and the rules are:
#
#   branding assets    restored from HEAD wholesale, as in conflict mode.
#
#   colour-managed     the palette swaps are *learned* from the staged diff --
#                      a changed line whose two sides are identical once the
#                      colour literals are blanked names one swap -- and then
#                      applied to the whole staged file.  Learning rather than
#                      hardcoding keeps one list of colours, in the skin, and
#                      repainting the whole file rather than reverting hunks
#                      also catches main's *new* rules: the FX pane arrived
#                      with 59 lines of fresh QSS written in main's palette,
#                      which no revert of an existing line would have caught.
#                      A literal main mapped two different ways is left alone
#                      and reported, since that is not a repaint.
#
#   main-only          listed in MAIN_ONLY_PATHS.  Development scaffolding that
#                      lives on main and has deliberately never been part of
#                      the release branch.  Unstaged and removed from the
#                      worktree when the merge is what added them, so the
#                      commit that follows does not carry them over.
#
#   --learn-from REV   learn the swaps from REV against HEAD instead of from
#                      the staged diff.  For a squash merged on the base it
#                      was last squashed from (tools/squash-to-bitedj.sh), REV
#                      being that base: the lines HEAD repainted are then not
#                      in the staged diff at all, so it has only main's new
#                      rules to learn from, which name main's colours and
#                      nothing else.  REV's file is main's palette and HEAD's
#                      is ours, so their diff is the repaint itself.
#
# A merge that conflicts in some files and takes the incoming side cleanly in
# others is both cases at once: resolve the conflicts, then run again with
# --mode staged to catch the branding that never conflicted.
#
# Files outside these lists are never touched, in either mode.  A file whose
# conflicts all resolve is staged; one with a block left is written back still
# conflicted and deliberately NOT staged.
#
# Exits 0 unless it could not write a file it was asked to resolve.
set -uo pipefail

# Branding assets: keep our version wholesale.  Globs allowed.
BRANDING_PATHS=(
    'res/skins/BiteDJ/images/bitedj_logo.png'
    'res/skins/BiteDJ/icons/effect_meter_fg.png'
)

# Files where a colour-only difference is branding and nothing more.  skin.xml
# is here because its <LaunchImageStyle> block is inline QSS.
COLOR_FILES=(
    'res/skins/BiteDJ/style.qss'
    'res/skins/BiteDJ/skin.xml'
)

# Staged mode only: files that exist on main and must not travel to the release
# branch with a squash merge.  Build-host and authoring tooling, none of it part
# of what ships on the appliance -- including this script, which is a merge tool
# for main's side of the merge and has no business in the release branch.
MAIN_ONLY_PATHS=(
    '.pre-commit-config.yaml'
    'cmake-build-release.sh'
    'res/skins/BiteDJ/.gitignore'
    'res/skins/BiteDJ/CLAUDE.md'
    'tools/resolve-branding.sh'
)

keep=ours
dryrun=0
mode=auto
learn_from=''

while [[ $# -gt 0 ]]; do
    case $1 in
        -n|--dry-run) dryrun=1; shift ;;
        --keep)       keep=${2:-}; shift 2 ;;
        --keep=*)     keep=${1#*=}; shift ;;
        --mode)       mode=${2:-}; shift 2 ;;
        --mode=*)     mode=${1#*=}; shift ;;
        --learn-from) learn_from=${2:-}; shift 2 ;;
        --learn-from=*) learn_from=${1#*=}; shift ;;
        -h|--help)    sed -n '2,/^set -uo/p' "$0" | sed 's/^# \{0,1\}//; $d'; exit 0 ;;
        --)           shift; break ;;
        -*)           echo "resolve-branding: unknown option $1" >&2; exit 64 ;;
        *)            break ;;
    esac
done

[[ $keep == ours || $keep == theirs ]] || {
    echo "resolve-branding: --keep takes 'ours' or 'theirs', not '$keep'" >&2
    exit 64
}

[[ $mode == auto || $mode == conflict || $mode == staged ]] || {
    echo "resolve-branding: --mode takes 'auto', 'conflict' or 'staged', not '$mode'" >&2
    exit 64
}

mapfile -t unmerged < <(git diff --name-only --diff-filter=U)

if [[ $mode == auto ]]; then
    # Unmerged paths mean a merge stopped on conflicts.  None of those, with
    # something staged, means it did not stop -- which is the squash merge
    # staged mode exists for.  Neither falls through to conflict mode, whose
    # "nothing unmerged" is the right thing to say.
    if [[ ${#unmerged[@]} -eq 0 ]] && ! git diff --cached --quiet; then
        mode=staged
    else
        mode=conflict
    fi
fi

matches() {  # matches <path> <pattern>...
    local path=$1 pat
    shift
    for pat in "$@"; do
        # shellcheck disable=SC2053
        [[ $path == $pat ]] && return 0
    done
    return 1
}

tmp=$(mktemp) && tmp2=$(mktemp) || exit 1
trap 'rm -f "$tmp" "$tmp2"' EXIT

# --- what counts as a colour ------------------------------------------------
#
# Shared by every awk program below, so the two modes cannot drift on what a
# colour literal is.

IFS='' read -r -d '' awk_color_lib <<'AWK' || true
# Blank every colour literal so two sides that differ only in palette compare
# equal.  \y is a word boundary, which keeps the hex rule off QSS object names:
# "#DeckBPM" has no hex run that ends on a boundary, so it is left alone.
function blank(l) {
    gsub(/rgba?\([^)]*\)/, "<c>", l)
    gsub(/#[0-9a-fA-F]{3,8}\y/, "<c>", l)
    return l
}

function colors(l, arr,   n, s) {
    n = 0
    delete arr
    s = l
    while (match(s, /rgba?\([^)]*\)|#[0-9a-fA-F]{3,8}\y/)) {
        arr[++n] = substr(s, RSTART, RLENGTH)
        s = substr(s, RSTART + RLENGTH)
    }
    return n
}
AWK

if [[ $mode == staged ]]; then
    # --- staged mode --------------------------------------------------------

    if [[ $keep == theirs ]]; then
        # The staged tree is already theirs: a clean merge took the incoming
        # side of every branding file without asking.
        echo "nothing to do: --keep theirs is what the merge already staged"
        exit 0
    fi

    if [[ $# -gt 0 ]]; then
        files=("$@")
    else
        mapfile -t files < <(git diff --cached --name-only)
    fi
    [[ ${#files[@]} -gt 0 ]] || { echo "nothing staged"; exit 0; }

    # This script's own path, relative to the repo root, or empty when it is
    # being run from outside the worktree it is operating on.  Dropping a
    # main-only file deletes it, and deleting the script we are running from is
    # left until everything else is done.
    self=''
    toplevel=$(git rev-parse --show-toplevel 2>/dev/null)
    script=$(realpath -- "$0" 2>/dev/null)
    [[ -n $toplevel && $script == "$toplevel"/* ]] && self=${script#"$toplevel"/}

    # --- the swap learner ---------------------------------------------------
    #
    # Reads `git diff -U0` for one file and prints one "<theirs>><ours>" per
    # colour main repainted, on stdout.  A changed line pairs with one on the
    # other side when the two are identical with their colours blanked out;
    # lines that pair with nothing are real changes and teach us nothing.
    # Pairing within the hunk rather than by position is what lets a hunk that
    # holds both a repaint and new rules still yield its repaint.

    IFS='' read -r -d '' learn_swaps <<'AWK' || true
function flush(   i, j, na, nb, a, b, n) {
    for (i = 1; i <= nt; i++) {
        for (j = 1; j <= no; j++) {
            if (used[j]) continue
            if (blank(theirs[i]) != blank(ours[j])) continue
            used[j] = 1
            na = colors(theirs[i], a)
            nb = colors(ours[j], b)
            for (n = 1; n <= na && n <= nb; n++)
                if (a[n] != b[n]) swap[tolower(a[n]) ">" b[n]]++
            break
        }
    }
    no = 0; nt = 0
    delete used
}

/^@@/                  { flush(); next }
/^(---|\+\+\+)/        { next }
/^-/                   { ours[++no]   = substr($0, 2); next }
/^\+/                  { theirs[++nt] = substr($0, 2); next }

function drop(k, why,   p) {
    split(k, p, ">")
    printf "  %s -> %s %s, left alone\n", p[1], p[2], why > "/dev/stderr"
    dead[k] = 1
}

# Every colour literal in the merged file, so a candidate pair can be checked
# against the whole of it and not just the lines that changed.
BEGIN {
    n = split(theirs_set, a, ";")
    for (i = 1; i <= n; i++) if (a[i] != "") theirs_has[a[i]] = 1
}

END {
    flush()

    # Two lines of the same shape are not yet evidence of a repaint.  Main
    # replacing a rule block wholesale reads exactly the same way -- the
    # #LibraryColumnSizeButton rewrite paired "border: 2px solid #855ea7" with
    # "border: 2px solid #2b3f4d" -- and repainting on that would revert a
    # deliberate change on the incoming side.  Two tests separate them.
    #
    # First, a repaint replaces every occurrence, so our colour is *gone* from
    # main's file -- nothing there still says #855ea7.  A colour main is still
    # using is one it means to use, whatever some line pairing suggests, which
    # is what drops #5c707d -> #a7a9ac: #a7a9ac is alive and well in main's new
    # rules.  Only this direction is tested.  The mirror image looks true but is
    # not: two stray #488ab3 survive our own repaint in style.qss, and a couple
    # of stragglers must not disqualify a swap with 26 lines behind it.
    for (k in swap) {
        split(k, p, ">")
        if (tolower(p[2]) in theirs_has) drop(k, "is still in use there")
    }


    # Second, one colour cannot have been repainted two ways, so where the
    # candidates disagree the one with the occurrences behind it wins.  That is
    # what drops #2b3f4d -> #855ea7: one line, against 26 saying #488ab3.  A
    # tie is not a repaint either way, so both go.
    for (k in swap) {
        if (k in dead) continue
        split(k, p, ">")
        best(p[1], k, bestf, tief)
        best(tolower(p[2]), k, bestt, tiet)
    }
    for (k in swap) {
        if (k in dead) continue
        split(k, p, ">")
        if (bestf[p[1]] != k || tief[p[1]])
            drop(k, "is outvoted for " p[1])
        else if (bestt[tolower(p[2])] != k || tiet[tolower(p[2])])
            drop(k, "is outvoted for " p[2])
    }

    for (k in swap) {
        if (k in dead) continue
        split(k, p, ">")
        printf "%s>%s\n", p[1], p[2]
        printf "  %-9s -> %-9s x%d\n", p[1], p[2], swap[k] > "/dev/stderr"
    }
}

# Tracks the highest-scoring candidate for one colour, and whether the top
# score is shared.
function best(key, k, top, tie) {
    if (!(key in top) || swap[k] > swap[top[key]]) {
        top[key] = k
        tie[key] = 0
    } else if (swap[k] == swap[top[key]] && top[key] != k) {
        tie[key] = 1
    }
}
AWK

    # --- the colour census --------------------------------------------------
    #
    # Every colour literal in one file, lowercased and deduplicated, as a
    # ";"-joined line.  Feeds the learner's "is it on the other side too" test.

    IFS='' read -r -d '' list_colors <<'AWK' || true
{
    n = colors($0, a)
    for (i = 1; i <= n; i++) seen[tolower(a[i])] = 1
}

END {
    for (c in seen) out = (out == "" ? c : out ";" c)
    print out
}
AWK

    # --- the repainter ------------------------------------------------------
    #
    # Rewrites every colour literal the learner mapped, anywhere in the file,
    # including in lines the merge added.  Walks the literals rather than
    # running gsub per pair so a colour is only ever substituted once.
    #
    # With --learn-from, only the lines the merge added: everything else is
    # HEAD's own text, already in the release palette, and HEAD still uses
    # some of main's colours on purpose -- 30 #488ab3 in style.qss at 1.0-3,
    # against the 4 lines its repaint changed.  `only` is the ","-joined list
    # of those line numbers; empty means every line.

    IFS='' read -r -d '' repaint_colors <<'AWK' || true
BEGIN {
    n = split(map, pairs, ";")
    for (i = 1; i <= n; i++)
        if (split(pairs[i], p, ">") == 2) to[p[1]] = p[2]
    n = split(only, a, ",")
    for (i = 1; i <= n; i++) pick[a[i]] = 1
}

only != "" && !(FNR in pick) { print; next }

{
    line = $0
    out = ""
    while (match(line, /rgba?\([^)]*\)|#[0-9a-fA-F]{3,8}\y/)) {
        lit = substr(line, RSTART, RLENGTH)
        key = tolower(lit)
        if (key in to) { lit = to[key]; hits++ }
        out = out substr(line, 1, RSTART - 1) lit
        line = substr(line, RSTART + RLENGTH)
    }
    print out line
}

END { printf "  %d literal(s) repainted\n", hits > "/dev/stderr" }
AWK

    repainted=() restored=() dropped=() untouched=() broken=() defer_self=0

    for f in "${files[@]}"; do
        if matches "$f" "${BRANDING_PATHS[@]}"; then
            git diff --cached --quiet -- "$f" && { untouched+=("$f"); continue; }
            if ! git cat-file -e "HEAD:$f" 2>/dev/null; then
                echo "resolve-branding: $f is new on main, not ours to restore" >&2
                untouched+=("$f")
                continue
            fi
            if (( dryrun )); then
                echo "would restore from HEAD: $f"
            else
                git checkout HEAD -- "$f" || { broken+=("$f"); continue; }
            fi
            restored+=("$f")

        elif matches "$f" "${COLOR_FILES[@]}"; then
            git diff --cached --quiet -- "$f" && { untouched+=("$f"); continue; }
            if ! git cat-file -e "HEAD:$f" 2>/dev/null || [[ ! -f $f ]] ||
                    { [[ -n $learn_from ]] && ! git cat-file -e "$learn_from:$f" 2>/dev/null; }; then
                untouched+=("$f")
                continue
            fi
            printf '\n\033[1m%s\033[0m\n' "$f"

            # The learner reads "-" lines as ours and "+" lines as theirs.
            if [[ -n $learn_from ]]; then
                theirs_set=$(git show "$learn_from:$f" | gawk "$awk_color_lib$list_colors")
                learn_diff=(git diff -U0 HEAD "$learn_from" -- "$f")
            else
                theirs_set=$(gawk "$awk_color_lib$list_colors" "$f")
                learn_diff=(git diff --cached -U0 -- "$f")
            fi
            mapfile -t swaps < <(
                "${learn_diff[@]}" |
                    gawk -v theirs_set="$theirs_set" \
                        "$awk_color_lib$learn_swaps")
            if [[ ${#swaps[@]} -eq 0 ]]; then
                echo "  no colour swap in the staged diff, left as merged"
                untouched+=("$f")
                continue
            fi

            joined=$(IFS=';'; echo "${swaps[*]}")
            only=''
            if [[ -n $learn_from ]]; then
                only=$(git diff --cached -U0 HEAD -- "$f" | gawk '
                    /^@@/ { match($0, /\+([0-9]+)(,([0-9]+))?/, m)
                            c = (m[3] == "") ? 1 : m[3]
                            for (i = 0; i < c; i++) printf "%s%d", (k++ ? "," : ""), m[1] + i }')
                [[ -n $only ]] || only=0
            fi
            if ! gawk -v map="$joined" -v only="$only" "$awk_color_lib$repaint_colors" "$f" > "$tmp"; then
                echo "  could not repaint, left as merged" >&2
                broken+=("$f")
                continue
            fi

            if (( dryrun )); then
                cmp -s "$tmp" <(git show "HEAD:$f") \
                    && echo "  would repaint back to HEAD's version and stage" \
                    || echo "  would repaint and stage (real changes kept)"
            else
                cat "$tmp" > "$f"        # in place, keeping mode and inode
                git add -- "$f" || { broken+=("$f"); continue; }
                git diff --cached --quiet -- "$f" \
                    && echo "  repainted back to HEAD's version" \
                    || echo "  repainted, real changes kept"
            fi
            repainted+=("$f")

        elif matches "$f" "${MAIN_ONLY_PATHS[@]}"; then
            if git cat-file -e "HEAD:$f" 2>/dev/null; then
                # Tracked on this branch already, so the merge is changing it
                # rather than carrying it over.  Not ours to delete.
                echo "resolve-branding: $f is tracked in HEAD, leaving it" >&2
                untouched+=("$f")
                continue
            fi
            if (( dryrun )); then
                echo "would drop (main only): $f"
            elif [[ -n $self && $f == "$self" ]]; then
                defer_self=1
            else
                git rm -q --cached --force -- "$f" && rm -f -- "$f" \
                    || { broken+=("$f"); continue; }
            fi
            dropped+=("$f")

        else
            untouched+=("$f")
        fi
    done

    # Last, and only now: this is the file bash is reading the script from.
    if (( defer_self )); then
        git rm -q --cached --force -- "$self" && rm -f -- "$self" \
            || broken+=("$self")
    fi

    printf '\n\033[1mbranding kept in %d file(s)\033[0m\n' \
        $(( ${#restored[@]} + ${#repainted[@]} ))
    (( ${#restored[@]} )) && printf '  restored from HEAD: %s\n' "${restored[@]}"
    (( ${#repainted[@]} )) && printf '  repainted: %s\n' "${repainted[@]}"
    (( ${#dropped[@]} )) && {
        printf 'dropped, main only:\n'
        printf '  %s\n' "${dropped[@]}"
    }
    (( ${#broken[@]} )) && printf 'left alone, needs a look: %s\n' "${broken[*]}"
    printf '\nnext: git diff --cached  then  git commit\n'

    (( ${#broken[@]} )) && exit 1
    exit 0
fi

# --- conflict mode ----------------------------------------------------------

if [[ $# -gt 0 ]]; then
    files=("$@")
else
    files=("${unmerged[@]}")
fi

[[ ${#files[@]} -gt 0 ]] || { echo "nothing unmerged"; exit 0; }

# Re-split a conflicted file from its index stages using the plain conflict
# style.  Stage 1 is missing for an add/add conflict; an empty base is correct
# there, and it is what produced the granular blocks in the first place.
# Prints the rebuilt file; returns 1 if the stages are not all there.
regen_plain() {
    local f=$1 o b t rc
    o=$(mktemp) && b=$(mktemp) && t=$(mktemp) || return 1
    if git show ":2:$f" > "$o" 2>/dev/null && git show ":3:$f" > "$t" 2>/dev/null; then
        git show ":1:$f" > "$b" 2>/dev/null || : > "$b"
        # merge-file exits with the conflict count, so only 255 means failure.
        git -c merge.conflictStyle=merge merge-file -p \
            -L "$ours_label" -L base -L "$theirs_label" "$o" "$b" "$t"
        rc=$?
        [[ $rc -eq 255 ]] && rc=1 || rc=0
    else
        rc=1
    fi
    rm -f "$o" "$b" "$t"
    return $rc
}

# --- the colour-blind conflict walker ---------------------------------------
#
# Reads a conflicted file, writes the resolved text to stdout and a one-line
# note per block to stderr.  Exit 0 when every block was branding, 1 when at
# least one block survived, 3 on malformed markers.

IFS='' read -r -d '' resolve_colors <<'AWK' || true
function fail(msg) {
    printf "resolve-branding: %s at %s:%d\n", msg, name, FNR > "/dev/stderr"
    exit 3
}

function same(   i, j, a, b, na, nb) {
    if (no != nt) return 0
    for (i = 1; i <= no; i++)
        if (blank(ours[i]) != blank(theirs[i])) return 0
    # Same shape: record which literal replaced which, for the summary.
    for (i = 1; i <= no; i++) {
        na = colors(ours[i], a)
        nb = colors(theirs[i], b)
        for (j = 1; j <= na && j <= nb; j++)
            if (a[j] != b[j]) swap[b[j] " -> " a[j]]++
    }
    return 1
}

function emit(arr, n,   i) { for (i = 1; i <= n; i++) print arr[i] }

function verbatim(   i) {
    print headmark
    emit(ours, no)
    if (basemark != "") { print basemark; emit(base, nb) }
    print "======="
    emit(theirs, nt)
    print tailmark
}

BEGIN { if (keep == "") keep = "ours"; if (name == "") name = FILENAME }

/^<<<<<<< / {
    if (in_o || in_b || in_t) fail("nested <<<<<<< marker")
    in_o = 1; no = 0; nb = 0; nt = 0
    headmark = $0; basemark = ""; start = FNR
    next
}

in_o && /^\|\|\|\|\|\|\|/ { in_o = 0; in_b = 1; basemark = $0; next }   # diff3
in_b && /^=======$/       { in_b = 0; in_t = 1; next }
in_o && /^=======$/       { in_o = 0; in_t = 1; next }

in_t && /^>>>>>>> / {
    in_t = 0; tailmark = $0; total++
    if (same()) {
        if (keep == "theirs") emit(theirs, nt); else emit(ours, no)
        branding++
    } else {
        verbatim()
        printf "  kept for review: %s:%d\n", name, start > "/dev/stderr"
    }
    next
}

in_o { ours[++no]   = $0; next }
in_b { base[++nb]   = $0; next }
in_t { theirs[++nt] = $0; next }

{ print }

END {
    if (in_o || in_b || in_t) fail("unterminated conflict block")
    for (k in swap) printf "  %-24s x%d\n", k, swap[k] > "/dev/stderr"
    printf "  %d/%d block(s) were branding only\n", branding, total > "/dev/stderr"
    exit (branding == total ? 0 : 1)
}
AWK

took=() partial=() untouched=() broken=()

for f in "${files[@]}"; do
    if matches "$f" "${BRANDING_PATHS[@]}"; then
        # Stage 2 is "ours", stage 3 is "theirs"; an add/add conflict has both.
        stage=2; [[ $keep == theirs ]] && stage=3
        if ! git cat-file -e ":$stage:$f" 2>/dev/null; then
            echo "resolve-branding: no '$keep' side for $f, leaving it" >&2
            broken+=("$f")
            continue
        fi
        if (( dryrun )); then
            echo "would keep $keep: $f"
        else
            git checkout "--$keep" -- "$f" && git add -- "$f" || { broken+=("$f"); continue; }
        fi
        took+=("$f")

    elif matches "$f" "${COLOR_FILES[@]}"; then
        if ! grep -qa '^<<<<<<< ' "$f"; then
            untouched+=("$f")
            continue
        fi
        printf '\n\033[1m%s\033[0m\n' "$f"

        # Keep whatever labels the merge already wrote into the file.
        ours_label=$(grep -am1 '^<<<<<<< ' "$f" | sed 's/^<<<<<<< //')
        theirs_label=$(grep -am1 '^>>>>>>> ' "$f" | sed 's/^>>>>>>> //')
        : "${ours_label:=HEAD}" "${theirs_label:=MERGE_HEAD}"

        src=$f
        if grep -qa '^|||||||' "$f"; then
            if regen_plain "$f" > "$tmp2"; then
                src=$tmp2
                echo "  re-split from index stages (diff3 gave one whole-file block)"
            else
                echo "  diff3 markers but no index stages; parsing as-is" >&2
            fi
        fi

        gawk -v keep="$keep" -v name="$f" "$awk_color_lib$resolve_colors" "$src" > "$tmp"
        case $? in
            0)  if (( dryrun )); then
                    echo "  would resolve fully and stage"
                else
                    cat "$tmp" > "$f"        # in place, keeping mode and inode
                    git add -- "$f"
                fi
                took+=("$f") ;;
            1)  if (( dryrun )); then
                    echo "  would resolve the branding blocks, leaving the rest"
                else
                    cat "$tmp" > "$f"
                fi
                partial+=("$f") ;;
            *)  echo "  bad markers, left untouched" >&2
                broken+=("$f") ;;
        esac

    else
        untouched+=("$f")
    fi
done

printf '\n\033[1mbranding resolved in %d file(s)\033[0m\n' "${#took[@]}"
(( ${#partial[@]} )) && {
    printf 'still conflicted (branding blocks dropped, real changes kept):\n'
    printf '  %s\n' "${partial[@]}"
}
(( ${#broken[@]} )) && printf 'left alone, needs a look: %s\n' "${broken[*]}"
(( ${#untouched[@]} )) && {
    printf 'not branding, for tools/resolve-conflicts.sh:\n'
    printf '  %s\n' "${untouched[@]}"
}
(( ${#partial[@]} + ${#untouched[@]} )) && \
    printf '\nnext: tools/resolve-conflicts.sh bitedj/main\n'

(( ${#broken[@]} )) && exit 1
exit 0
