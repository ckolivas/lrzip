#!/bin/sh
# Recursive lists must retain every file across nested and multiple inputs.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LRZIP=${LRZIP:-$ROOT/lrzip}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/lrzip-recursive.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/tree/nested/deeper" "$WORK/second tree" "$WORK/empty"
printf 'top-level file\n' > "$WORK/tree/top"
printf 'nested file\n' > "$WORK/tree/nested/file with spaces"
printf 'deep file\n' > "$WORK/tree/nested/deeper/file"
: > "$WORK/tree/empty"
printf 'second input directory\n' > "$WORK/second tree/file"
cp -R "$WORK/tree" "$WORK/expected"
cp -R "$WORK/second tree" "$WORK/second expected"

"$LRZIP" -Q -n -r "$WORK/tree" "$WORK/second tree/" "$WORK/empty"
for file in top 'nested/file with spaces' nested/deeper/file empty; do
	test -f "$WORK/tree/$file.lrz"
	rm "$WORK/tree/$file"
done
test -f "$WORK/second tree/file.lrz"
rm "$WORK/second tree/file"
"$LRZIP" -Q -d -r "$WORK/tree" "$WORK/second tree/" "$WORK/empty"
for file in top 'nested/file with spaces' nested/deeper/file empty; do
	cmp "$WORK/expected/$file" "$WORK/tree/$file"
done
cmp "$WORK/second expected/file" "$WORK/second tree/file"
echo "Recursive directory round-trips passed"
