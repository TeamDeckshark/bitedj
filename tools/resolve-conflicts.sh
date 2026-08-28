#!/usr/bin/env bash
#
# resolve-conflicts.sh [branch] [file ...]
#
# Drives resolve-conflicts.awk over every unmerged file (or the files named on
# the command line), rewrites each in place with the chosen sides, and stages
# the ones that came out clean.  Files without conflict markers (binaries from
# an "added by both") are listed at the end for manual handling.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
awkfile=$here/resolve-conflicts.awk

branch=${1:-main}
shift || true

if [[ $# -gt 0 ]]; then
    files=("$@")
else
    mapfile -t files < <(git diff --name-only --diff-filter=U)
fi

[[ ${#files[@]} -gt 0 ]] || { echo "nothing unmerged"; exit 0; }

tmp=$(mktemp) || exit 1
trap 'rm -f "$tmp"' EXIT

skipped=() resolved=() failed=()

for f in "${files[@]}"; do
    if ! grep -qa '^<<<<<<< ' "$f"; then
        skipped+=("$f")
        continue
    fi
    gawk -v branch="$branch" -f "$awkfile" "$f" > "$tmp"
    case $? in
        0)  cat "$tmp" > "$f"          # rewrite in place, keeping mode/inode
            git add -- "$f"
            resolved+=("$f") ;;
        2)  echo "aborted; $f left untouched" >&2
            break ;;
        *)  failed+=("$f") ;;
    esac
done

printf '\n\033[1mresolved %d file(s)\033[0m\n' "${#resolved[@]}"
[[ ${#failed[@]}  -gt 0 ]] && printf 'bad markers, left alone: %s\n' "${failed[*]}"
[[ ${#skipped[@]} -gt 0 ]] && {
    printf 'no text conflict (resolve by hand, e.g. git checkout --ours/--theirs):\n'
    printf '  %s\n' "${skipped[@]}"
}
exit 0
