#!/bin/sh
# A long match must remain correct when both operands need history maps.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LRZIP=${LRZIP:-$ROOT/lrzip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lrzip-sliding.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

page=$(getconf PAGESIZE)
window=$((100 * 1024 * 1024 / 3 / page * page))
dd if=/dev/urandom of="$WORK/input" bs=1048576 count=80 2>/dev/null
dd if="$WORK/input" of="$WORK/repeated" bs="$page" \
	skip=$((1048576 / page)) count=2 2>/dev/null
# The repeat crosses the end of the second main mapping. Its source has
# already fallen out of that mapping, so two history pages must coexist.
dd if="$WORK/repeated" of="$WORK/input" bs="$page" \
	seek=$((2 * window / page - 1)) conv=notrunc 2>/dev/null
"$LRZIP" -Q -n -U -m 1 -L 1 -p 1 -o "$WORK/archive.lrz" "$WORK/input"
"$LRZIP" -Q -d -c -o "$WORK/output" "$WORK/archive.lrz"
cmp "$WORK/input" "$WORK/output"
echo "Sliding match boundary round-trip passed"
