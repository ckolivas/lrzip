#!/bin/bash
# Encrypt round-trip matrix for modern AEAD (default -e) and --legacy-encrypt.
#
# Copyright (C) 2026 Con Kolivas
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LRZ="${LRZ:-$ROOT/lrzip}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

printf 'lrzip encrypt test payload %s\n' "$(seq 1 200 | tr '\n' ' ')" > plain.txt
cp plain.txt plain.orig

echo "== modern AEAD (default -e) =="
"$LRZ" -f --encrypt=testpass -o modern.lrz plain.txt >/dev/null
python3 - <<'PY'
import struct
d = open("modern.lrz", "rb").read(56)
assert d[0:4] == b"LRZI", d[0:4]
assert d[22] == 3, d[22]
assert d[24] == 1 and d[25] == 16
assert struct.unpack_from("<I", d, 26)[0] == 600000
print("header ok: mode=3 suite=1 iters=600000")
PY
"$LRZ" -df --encrypt=testpass modern.lrz >/dev/null
cmp plain.txt plain.orig
echo "modern round-trip OK"

echo "== legacy 0.6-compatible =="
cp plain.orig plain.txt
"$LRZ" -f --encrypt=testpass --legacy-encrypt -o legacy.lrz plain.txt >/dev/null
python3 - <<'PY'
d = open("legacy.lrz", "rb").read(24)
assert d[22] == 1, d[22]
print("header ok: mode=1")
PY
"$LRZ" -df --encrypt=testpass legacy.lrz >/dev/null
cmp plain.txt plain.orig
echo "legacy round-trip OK"

echo "== wrong password fails closed =="
set +e
"$LRZ" -df --encrypt=wrong modern.lrz >/dev/null 2>wrong.err
rc=$?
set -e
grep -qiE 'authentication|AEAD|HMAC|wrong password|Fatal|failed' wrong.err
echo "wrong password rejected (rc=$rc)"

echo "== ciphertext tamper fails closed =="
python3 - <<'PY'
d = bytearray(open("modern.lrz", "rb").read())
d[min(len(d) - 1, 80)] ^= 0x5A
open("tamper.lrz", "wb").write(d)
PY
set +e
"$LRZ" -df --encrypt=testpass tamper.lrz >/dev/null 2>tamper.err
set -e
grep -qiE 'authentication|AEAD|HMAC|Fatal|failed|corrupt' tamper.err
echo "tamper rejected"

echo "All encrypt tests passed."
