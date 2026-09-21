#!/bin/sh
# Changing input sizes under a memory limit must not leave stale worker slots.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LRZIP=${LRZIP:-$ROOT/lrzip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lrzip-threads.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

dd if=/dev/urandom of="$WORK/half" bs=1048576 count=3 2>/dev/null
cat "$WORK/half" "$WORK/half" > "$WORK/small"
dd if=/dev/urandom of="$WORK/large" bs=1048576 count=100 2>/dev/null
# The small file keeps three slots; the large file reduces the pool to one.
# Repeating both inputs also verifies restarting a larger pool afterwards.
"$LRZIP" -f -L 1 -m 1 -p 2 "$WORK/small" "$WORK/large" \
	"$WORK/small" "$WORK/large" > "$WORK/log" 2>&1 || {
	cat "$WORK/log" >&2
	exit 1
}
grep -q 'Minimising number of threads to 1' "$WORK/log"
for file in small large; do
	"$LRZIP" -Q -d -o "$WORK/restored-$file" "$WORK/$file.lrz"
	cmp "$WORK/$file" "$WORK/restored-$file"
done
echo "Compression thread-count reduction round-trips passed"
