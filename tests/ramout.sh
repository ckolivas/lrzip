#!/bin/sh
# File reconstruction must preserve overflow, -c and --keep-broken behavior.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LRZIP=${LRZIP:-$ROOT/lrzip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lrzip-ramout.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# A short-period input has small encoded blocks but exceeds the -m1
# output buffer, forcing the cache to spill into the ordinary output file.
yes abc 2>/dev/null | head -c 67108864 > "$WORK/input"
"$LRZIP" -Q -n -L 7 -o "$WORK/archive.lrz" "$WORK/input"
"$LRZIP" -Q -d -m 1 -c -o "$WORK/output" "$WORK/archive.lrz"
cmp "$WORK/input" "$WORK/output"

# A digest error must still leave partial output when -K requests it.
cp "$WORK/archive.lrz" "$WORK/bad.lrz"
size=$(wc -c < "$WORK/bad.lrz")
byte=$(tail -c 1 "$WORK/bad.lrz" | od -An -tu1)
printf '%b' "$(printf '\\%03o' "$((byte ^ 1))")" |
	dd of="$WORK/bad.lrz" bs=1 seek="$((size - 1))" conv=notrunc 2>/dev/null
if "$LRZIP" -Q -f -d -K -o "$WORK/output" "$WORK/bad.lrz" > /dev/null 2>&1; then
	echo "Corrupt archive unexpectedly passed" >&2
	exit 1
fi
cmp "$WORK/input" "$WORK/output"
echo "File reconstruction overflow and keep-broken tests passed"
