#!/bin/sh
# Exercise MD5 handoff, full batches, and partial final batches.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LRZIP=${LRZIP:-$ROOT/lrzip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lrzip-checksum.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

python3 - "$WORK/data" <<'PY'
import random, sys
with open(sys.argv[1], 'wb') as out:
    out.write(random.Random(9831).randbytes(3 * 1024 * 1024 + 1))
PY
for size in 0 1 1048575 1048576 1048577 2097151 2097152 2097153 3145729; do
	head -c "$size" "$WORK/data" > "$WORK/input"
	"$LRZIP" -Q -f -n -p 2 -o "$WORK/archive.lrz" "$WORK/input"
	"$LRZIP" -Q -f -d -p 2 -c -o "$WORK/output" "$WORK/archive.lrz"
	cmp "$WORK/input" "$WORK/output"
	"$LRZIP" -Q -d -p 2 -o - "$WORK/archive.lrz" > "$WORK/stdout"
	cmp "$WORK/input" "$WORK/stdout"
done
echo "MD5 batch boundary tests passed"
