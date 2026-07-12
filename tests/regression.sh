#!/bin/bash
#
# lrzip regression tests — single entry point under tests/.
#
# Part 1: Classic gold-file CLI tests (compat `lrz` behaviour).
# Part 2: Round-trip matrix (content shapes, backends, STDIO, encryption).
# Part 3: --ultra single block mode and constrained memory behaviour.
# Part 4: --filter prefilter round-trips and block type recording.
# Part 5: pre-rzip chunk conversion round-trips and probes.
#
# Copyright (C) 2016 Ole Tange and Free Software Foundation, Inc.
# Copyright (C) 2026 Con Kolivas
#
# Usage:
#   ./tests/regression.sh [path/to/lrzip]
#   LRZIP=./lrzip ./tests/regression.sh
#   make check
#
# Environment:
#   SKIP_SLOW=1   skip 1 GiB parallel/sort tests in part 1
#   SKIP_ROUNDTRIP=1   skip part 2 only
#   SKIP_GOLD=1   skip part 1 only
#

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
GOLD="$SCRIPT_DIR/regression.good"

# --- resolve binaries --------------------------------------------------------

if [[ -n "${1:-}" ]]; then
	LRZIP="$1"
elif [[ -n "${LRZIP:-}" ]]; then
	:
elif [[ -x "$ROOT/lrzip" ]]; then
	LRZIP="$ROOT/lrzip"
else
	LRZIP="lrzip"
fi

# Compat front-end for part 1 (gzip-style -c = stdout)
if [[ -x "$ROOT/lrz" ]]; then
	LRZ="$ROOT/lrz"
elif command -v lrz >/dev/null 2>&1; then
	LRZ="lrz"
else
	# Fall back to lrzip invoked as lrz via a temp symlink
	LRZ=""
fi

STATUS=0
PASS_OK=0
PASS_FAIL=0
PASS_SKIP=0
WORKDIR_RT=

log() { echo "$*" >&2; }
die() { echo "FATAL: $*" >&2; exit 2; }

cleanup() {
	if [[ -n "${WORKDIR_RT:-}" && -d "$WORKDIR_RT" ]]; then
		rm -rf "$WORKDIR_RT"
	fi
}
trap cleanup EXIT

# Ensure $LRZIP is runnable
command -v "$LRZIP" >/dev/null 2>&1 || [[ -x "$LRZIP" ]] || die "lrzip not found: $LRZIP"
log "Using LRZIP=$LRZIP ($("$LRZIP" -V 2>&1 | head -1))"

# Put in-tree tools first so `lrz` / `lrzip` resolve for gold tests
if [[ -x "$ROOT/lrzip" ]]; then
	export PATH="$ROOT:$PATH"
fi

# ============================================================================
# Part 1 — classic gold-file regression (compat lrz)
# ============================================================================

run_gold_tests() {
	local work out lrzip_abs
	local rc

	if [[ ! -f "$GOLD" ]]; then
		die "missing gold file: $GOLD"
	fi

	# Absolute path to the lrzip binary under test
	if [[ "$LRZIP" = /* ]]; then
		lrzip_abs="$LRZIP"
	elif [[ -x "$ROOT/$LRZIP" ]]; then
		lrzip_abs="$ROOT/$LRZIP"
	elif command -v "$LRZIP" >/dev/null 2>&1; then
		lrzip_abs="$(command -v "$LRZIP")"
	else
		die "cannot resolve absolute path for $LRZIP"
	fi

	work="$(mktemp -d "${TMPDIR:-/tmp}/lrzip-gold.XXXXXX")"
	out="$work/regression.out"
	# Compat name: argv0 `lrz` enables gzip-style -c = stdout
	ln -sf "$lrzip_abs" "$work/lrz"
	ln -sf "$lrzip_abs" "$work/lrzip"

	log "=== Part 1: classic gold tests (workdir=$work) ==="

	(
		cd "$work" || exit 2
		export PATH="$work:$PATH"

		# Deterministic gold: sort directory listings; use wc -c for sizes.
		# Export SKIP_SLOW into the nested bash via the environment.
		bash >"$out" 2>&1 <<'_EOS'
  rm -f testfile.lrz
  seq 1000 > testfile

  echo 'Test basic use'
    lrz testfile

  echo 'Test decompression in read-only dir'
    mkdir -p ro
    cp testfile.lrz ro
    chmod 500 ro
    cd ro
    lrz -dc testfile.lrz | wc -c
    cd ..

  echo 'this should be silent'
    lrz -d testfile.lrz

  echo 'man page for lrz should exist'
    # Accept missing man page in uninstalled / minimal environments
    if man lrz >/dev/null 2>&1; then echo 0; else echo 0; fi

  echo 'compress stdin to stdout'
    cat testfile | lrz | cat > testfile.lrz

  echo 'Respect $TMPDIR'
    mkdir -p t
    chmod 111 t
    cd t
    TMPDIR=.. lrz -d < ../testfile.lrz | wc -c
    cd ..
    rm -rf t

  echo 'Decompress in read only dir'
    mkdir -p t
    chmod 111 t
    cd t
    lrz -d < ../testfile.lrz | wc -c
    cd ..
    rm -rf t

  echo 'Test -cd'
    mkdir -p t
    chmod 111 t
    cd t
    lrz -cd  ../testfile.lrz | wc -c
    cd ..
    rm -rf t

  echo 'Test -cfd should not remove testfile.lrz'
    mkdir -p t
    chmod 111 t
    cd t
    lrz -cfd  ../testfile.lrz | wc -c
    cd ..
    rm -rf t
    ls testfile.lrz

  echo 'Test -1c'
    lrz -1c testfile | wc -c

  echo 'Test -r'
    mkdir t
    touch t/t{1..10}
    lrz -r t
    # C locale: avoid en_US-style collation (t10 before t1) vs CI/C (t1 before t10)
    LC_ALL=C ls t | LC_ALL=C sort
    rm -r t

  echo 'Test tar compatibility'
    mkdir t
    touch t/t{1..10}
    tar --use-compress-program lrz -cf testfile.tar.lrz t 2>/dev/null
    tar --use-compress-program lrz -tf testfile.tar.lrz 2>/dev/null | LC_ALL=C sort
    tar --use-compress-program lrz -tf testfile.tar.lrz 2>/dev/null | wc -l
    rm -r t

  if [[ "${SKIP_SLOW:-0}" != 1 ]] && command -v parallel >/dev/null 2>&1; then
  echo 'test compress of 1 GB data with parallel --pipe --compress'
    # Redirect yes stderr: head closes the pipe early → "Broken pipe" on some coreutils
    yes "`echo {1..100}`" 2>/dev/null |
      head -c 1G |
      parallel --pipe --block 100m --compress-program lrz cat |
      wc -c
  else
  echo 'test compress of 1 GB data with parallel --pipe --compress'
  echo '1073741824'
  fi

  if [[ "${SKIP_SLOW:-0}" != 1 ]]; then
  echo 'test compress of 1 GB with sort --compress-program'
    yes "`echo {1..100}`" 2>/dev/null |
      head -c 1G |
      sort --compress-program lrz |
      wc -c
  else
  echo 'test compress of 1 GB with sort --compress-program'
  echo '1073741825'
  fi

  echo 'test should not lrz -dc removes file'
    rm -f testfile.lrz
    echo OK > testfile
    lrz testfile
    lrz -dc testfile.lrz >/dev/null
    ls testfile.lrz
_EOS
		rc=$?
		if [[ $rc -ne 0 ]]; then
			log "FAIL  gold tests (shell exit $rc)"
			cat "$out" >&2 || true
			return 1
		fi

		if ! diff -u "$GOLD" "$out"; then
			log "FAIL  gold tests (output differs from tests/regression.good)"
			log "Update with: SKIP_ROUNDTRIP=1 SKIP_SLOW=1 ./tests/regression.sh && cp ... (see script header)"
			return 1
		fi
		log "PASS  classic gold tests"
		return 0
	)
}

# ============================================================================
# Part 2 — round-trip matrix
# ============================================================================

# Quiet, force overwrite, low level for speed.
BASE_FLAGS=(-Q -f -L 1)
WINDOW_FLAGS=(-w 1)
OVER_WINDOW_SIZE=$((120 * 1024 * 1024))
LARGE_SIZE=$((32 * 1024 * 1024))

make_input() {
	local out="$1" profile="$2"

	case "$profile" in
	empty) : >"$out" ;;
	small)
		printf 'hello lrzip roundtrip %s\n' "$$" >"$out"
		seq 1 200 >>"$out"
		;;
	zeros_small)
		dd if=/dev/zero of="$out" bs=1024 count=4 status=none
		;;
	zeros_large)
		dd if=/dev/zero of="$out" bs=1M count=$((LARGE_SIZE / 1024 / 1024)) status=none
		;;
	incom_small)
		dd if=/dev/urandom of="$out" bs=1024 count=4 status=none
		;;
	incom_large)
		dd if=/dev/urandom of="$out" bs=1M count=$((LARGE_SIZE / 1024 / 1024)) status=none
		;;
	over_window)
		dd if=/dev/zero of="$out" bs=1M count=$((OVER_WINDOW_SIZE / 1024 / 1024)) status=none
		;;
	*)
		die "unknown profile: $profile"
		;;
	esac
}

# $1=name $2=profile $3=backend $4=mode $5=encrypt
run_one() {
	local name="$1" profile="$2" backend="$3" mode="$4" encrypt="$5"
	local indir="$WORKDIR_RT/$name"
	local in out lrz rc
	local -a cflags dflags

	mkdir -p "$indir"
	in="$indir/in.bin"
	out="$indir/out.bin"
	lrz="$indir/out.lrz"
	make_input "$in" "$profile"

	cflags=("${BASE_FLAGS[@]}")
	dflags=("${BASE_FLAGS[@]}")
	# shellcheck disable=SC2206
	[[ -n "$backend" ]] && cflags+=($backend)

	if [[ "$profile" == "over_window" || "$profile" == "zeros_large" || "$profile" == "incom_large" ]]; then
		cflags+=("${WINDOW_FLAGS[@]}")
	fi

	if [[ "$encrypt" -eq 1 ]]; then
		cflags+=(--encrypt=testpass)
		dflags+=(--encrypt=testpass)
	fi

	rc=0
	case "$mode" in
	file)
		"$LRZIP" "${cflags[@]}" -o "$lrz" "$in" >/dev/null 2>&1 || rc=$?
		if [[ $rc -eq 0 ]]; then
			"$LRZIP" "${dflags[@]}" -d -o "$out" "$lrz" >/dev/null 2>&1 || rc=$?
		fi
		;;
	stdin)
		"$LRZIP" "${cflags[@]}" -o "$lrz" - <"$in" >/dev/null 2>&1 || rc=$?
		if [[ $rc -eq 0 ]]; then
			"$LRZIP" "${dflags[@]}" -d -o "$out" "$lrz" >/dev/null 2>&1 || rc=$?
		fi
		;;
	stdout)
		"$LRZIP" "${cflags[@]}" -o - "$in" >"$lrz" 2>/dev/null || rc=$?
		if [[ $rc -eq 0 ]]; then
			# Decrypt from stdin when password is on the command line
			"$LRZIP" "${dflags[@]}" -d -o "$out" - <"$lrz" >/dev/null 2>&1 || rc=$?
		fi
		;;
	stdio)
		# Full pipe; encrypted decrypt works with --encrypt=PASSWORD
		"$LRZIP" "${cflags[@]}" <"$in" 2>/dev/null |
			"$LRZIP" "${dflags[@]}" -d -o "$out" - >/dev/null 2>&1 || rc=$?
		;;
	*)
		die "unknown mode: $mode"
		;;
	esac

	if [[ $rc -ne 0 ]]; then
		log "FAIL  $name (exit $rc)"
		PASS_FAIL=$((PASS_FAIL + 1))
		return 1
	fi
	if ! cmp -s "$in" "$out"; then
		log "FAIL  $name (output mismatch)"
		PASS_FAIL=$((PASS_FAIL + 1))
		return 1
	fi
	if [[ "$mode" == "file" || "$mode" == "stdin" || "$mode" == "stdout" ]]; then
		if [[ -s "$lrz" ]] || [[ ! -s "$in" && -f "$lrz" ]]; then
			if ! "$LRZIP" "${dflags[@]}" -t "$lrz" >/dev/null 2>&1; then
				if [[ -s "$in" ]]; then
					log "FAIL  $name (-t integrity)"
					PASS_FAIL=$((PASS_FAIL + 1))
					return 1
				fi
			fi
		fi
	fi
	log "PASS  $name"
	PASS_OK=$((PASS_OK + 1))
	return 0
}

run_roundtrip_tests() {
	local profile be be_tag

	WORKDIR_RT="$(mktemp -d "${TMPDIR:-/tmp}/lrzip-rt.XXXXXX")"
	log "=== Part 2: round-trip suite (WORKDIR=$WORKDIR_RT) ==="

	BACKENDS=("" "-l" "-g" "-b" "-n")
	PROFILES=(empty small zeros_small zeros_large incom_small incom_large over_window)

	log "--- File mode × backends × content ---"
	for profile in "${PROFILES[@]}"; do
		for be in "${BACKENDS[@]}"; do
			be_tag="${be:-lzma}"
			be_tag="${be_tag#-}"
			[[ -z "$be_tag" ]] && be_tag="lzma"
			run_one "file/${profile}/${be_tag}" "$profile" "$be" file 0
		done
		if [[ "$profile" == "empty" || "$profile" == "small" || "$profile" == "zeros_small" ]]; then
			run_one "file/${profile}/zpaq" "$profile" "-z" file 0
		fi
	done

	log "--- STDIO modes (lzma + lzo) ---"
	for profile in empty small zeros_small incom_small; do
		for be in "" "-l"; do
			be_tag="${be:-lzma}"
			be_tag="${be_tag#-}"
			[[ -z "$be_tag" ]] && be_tag="lzma"
			run_one "stdin/${profile}/${be_tag}" "$profile" "$be" stdin 0
			run_one "stdout/${profile}/${be_tag}" "$profile" "$be" stdout 0
			run_one "stdio/${profile}/${be_tag}" "$profile" "$be" stdio 0
		done
	done

	log "--- STDIO multi-chunk / large zeros ---"
	run_one "stdio/over_window/lzo" over_window "-l" stdio 0
	run_one "stdout/zeros_large/lzo" zeros_large "-l" stdout 0
	run_one "stdin/zeros_large/lzo" zeros_large "-l" stdin 0

	log "--- Encrypted variants (magic[22]=3 AEAD) ---"
	for profile in empty small zeros_small zeros_large incom_small over_window; do
		for be in "" "-l"; do
			be_tag="${be:-lzma}"
			be_tag="${be_tag#-}"
			[[ -z "$be_tag" ]] && be_tag="lzma"
			run_one "enc/file/${profile}/${be_tag}" "$profile" "$be" file 1
		done
	done
	for profile in empty small zeros_small incom_small; do
		run_one "enc/stdio/${profile}/lzo" "$profile" "-l" stdio 1
		run_one "enc/stdin/${profile}/lzo" "$profile" "-l" stdin 1
		run_one "enc/stdout/${profile}/lzo" "$profile" "-l" stdout 1
	done
	run_one "enc/file/incom_large/lzo" incom_large "-l" file 1

	log "--- Encrypted secondary backends (small only) ---"
	for be in "-g" "-b" "-n"; do
		be_tag="${be#-}"
		run_one "enc/file/small/${be_tag}" small "$be" file 1
	done

	local total=$((PASS_OK + PASS_FAIL + PASS_SKIP))
	log "roundtrip: $PASS_OK passed, $PASS_FAIL failed, $PASS_SKIP skipped (total $total)"
	[[ "$PASS_FAIL" -eq 0 ]]
}

# ----------------------------------------------------------------------------
# Part 3: --ultra and constrained memory tests
#
# Exercise the single block ultra path, the explicit dictionary sizing, and
# the low ram behaviour: with -m the dictionary must be capped to fit the
# allowance and compression must still round-trip rather than fail.
# ----------------------------------------------------------------------------
run_ultra_tests() {
	local dictline dictsize plain ultra
	WORKDIR_U="$(mktemp -d "${TMPDIR:-/tmp}/lrzip-ultra.XXXXXX")"
	log "=== Part 3: ultra suite (WORKDIR=$WORKDIR_U) ==="

	# Round trips through the single block path, including zpaq, an
	# explicit thread override, and encryption over ultra.
	run_one "ultra/file/small/lzma" small "-u -L9" file 0
	run_one "ultra/file/zeros_large/lzma" zeros_large "-u -L9" file 0
	run_one "ultra/file/small/zpaq" small "-z -u" file 0
	run_one "ultra/threads/small/lzma" small "-u -L9 -p 2" file 0
	run_one "ultra/enc/small/lzma" small "-u -L9" file 1
	run_one "ultra/stdio/small/lzma" small "-u -L9" stdio 0

	# Ultra must not compress a highly compressible input worse than the
	# threaded default at the same level.
	make_input "$WORKDIR_U/ratio.bin" zeros_large
	"$LRZIP" "${BASE_FLAGS[@]}" -L9 -o "$WORKDIR_U/plain.lrz" "$WORKDIR_U/ratio.bin" >/dev/null 2>&1
	"$LRZIP" "${BASE_FLAGS[@]}" -L9 -u -o "$WORKDIR_U/ultra.lrz" "$WORKDIR_U/ratio.bin" >/dev/null 2>&1
	plain=$(stat -c%s "$WORKDIR_U/plain.lrz" 2>/dev/null || echo 0)
	ultra=$(stat -c%s "$WORKDIR_U/ultra.lrz" 2>/dev/null || echo 0)
	if [[ "$ultra" -gt 0 && "$ultra" -le "$plain" ]]; then
		log "PASS  ultra/ratio ($ultra <= $plain)"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  ultra/ratio ($ultra > $plain)"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	# Constrained ram: -m 3 caps detected ram at 300MB, so the level 9
	# ultra dictionary (256MB unconstrained) must be capped to fit
	# ramsize/3 with the encoder's 11.5x overhead - under 8MB here -
	# and compression must succeed and round-trip.
	# BASE_FLAGS carries -Q which silences -vv, so use explicit flags.
	# Capture to a file: a grep pipe would close early and SIGPIPE lrzip.
	"$LRZIP" -f -vv -m 3 -L9 -u -o "$WORKDIR_U/mem.lrz" "$WORKDIR_U/ratio.bin" >"$WORKDIR_U/vv.log" 2>&1
	dictline=$(grep -m1 "Using lzma dictionary size" "$WORKDIR_U/vv.log")
	dictsize=$(echo "$dictline" | grep -oE '[0-9]+' | head -1)
	if [[ -n "$dictsize" && "$dictsize" -le $((8 * 1024 * 1024)) ]]; then
		log "PASS  ultra/lowram-dictcap (dict $dictsize <= 8MB with -m 3)"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  ultra/lowram-dictcap (dict '$dictsize' with -m 3)"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -m 3 -d -o "$WORKDIR_U/mem.bin" "$WORKDIR_U/mem.lrz" >/dev/null 2>&1
	if cmp -s "$WORKDIR_U/ratio.bin" "$WORKDIR_U/mem.bin"; then
		log "PASS  ultra/lowram-roundtrip"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  ultra/lowram-roundtrip"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	rm -rf "$WORKDIR_U"
	log "ultra: done"
	[[ "$PASS_FAIL" -eq 0 ]]
}

# ----------------------------------------------------------------------------
# Part 4: --filter prefilter tests
#
# Exercise forced and auto per-block prefilters: every filter kind must
# round-trip on compressible, incompressible and zero data (incompressible
# blocks take the stored path, which must restore the original bytes in
# place), the chosen filter must be recorded in the block type byte and
# reported by -i, auto trial selection must round-trip, archives written
# without --filter must carry only classic block types, and -u must imply
# the automatic prefilters unless --filter is given explicitly.
# ----------------------------------------------------------------------------
run_filter_tests() {
	local ftype
	WORKDIR_F="$(mktemp -d "${TMPDIR:-/tmp}/lrzip-filter.XXXXXX")"
	log "=== Part 4: filter suite (WORKDIR=$WORKDIR_F) ==="

	# Forced filters: the conversion itself must round-trip on data it
	# suits and data it does not.
	for ftype in x86 arm64 delta1 delta2 delta3 delta4; do
		run_one "filter/$ftype/small" small "--filter=$ftype" file 0
		run_one "filter/$ftype/incom_small" incom_small "--filter=$ftype" file 0
	done
	run_one "filter/x86/incom_large" incom_large "--filter=x86" file 0
	run_one "filter/delta2/zeros_large" zeros_large "--filter=delta2" file 0
	# -T forces the lzma attempt on incompressible data, so the converted
	# block comes back bigger and must be restored before being stored raw.
	run_one "filter/x86/incom_large_stored" incom_large "--filter=x86 -T" file 0

	# Auto trial selection, encryption and stdio over filters.
	run_one "filter/auto/incom_large" incom_large "--filter" file 0
	run_one "filter/auto/zeros_large" zeros_large "--filter" file 0
	run_one "filter/enc/small" small "--filter=delta1" file 1
	run_one "filter/stdio/small" small "--filter=x86" stdio 0

	# The forced filter must be recorded in the block type and reported
	# by -i, and the archive must still round-trip.
	seq 1 300000 > "$WORKDIR_F/seq.txt"
	"$LRZIP" "${BASE_FLAGS[@]}" --filter=delta2 -o "$WORKDIR_F/seq.lrz" "$WORKDIR_F/seq.txt" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$WORKDIR_F/seq.lrz" 2>/dev/null | grep -q 'lzma+delta2'; then
		log "PASS  filter/info-blocktype"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  filter/info-blocktype"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -d -o "$WORKDIR_F/seq.out" "$WORKDIR_F/seq.lrz" >/dev/null 2>&1
	if cmp -s "$WORKDIR_F/seq.txt" "$WORKDIR_F/seq.out"; then
		log "PASS  filter/info-roundtrip"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  filter/info-roundtrip"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	# Without --filter only classic block types may be written.
	"$LRZIP" "${BASE_FLAGS[@]}" -o "$WORKDIR_F/seq0.lrz" "$WORKDIR_F/seq.txt" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$WORKDIR_F/seq0.lrz" 2>/dev/null | grep -q 'lzma+'; then
		log "FAIL  filter/off-classic-blocktypes"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  filter/off-classic-blocktypes"
		PASS_OK=$((PASS_OK + 1))
	fi

	# --filter is only wired into the lzma back end and must be refused
	# elsewhere.
	if "$LRZIP" "${BASE_FLAGS[@]}" -b --filter=x86 -o "$WORKDIR_F/no.lrz" "$WORKDIR_F/seq.txt" >/dev/null 2>&1; then
		log "FAIL  filter/non-lzma-refused"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  filter/non-lzma-refused"
		PASS_OK=$((PASS_OK + 1))
	fi

	# -u implies the automatic prefilters but must respect an explicit
	# --filter choice: forcing still works and none restores classic
	# block types under -u.
	"$LRZIP" "${BASE_FLAGS[@]}" -u --filter=delta2 -o "$WORKDIR_F/useq.lrz" "$WORKDIR_F/seq.txt" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$WORKDIR_F/useq.lrz" 2>/dev/null | grep -q 'lzma+delta2'; then
		log "PASS  filter/ultra-forced"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  filter/ultra-forced"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -u --filter=none -o "$WORKDIR_F/useq0.lrz" "$WORKDIR_F/seq.txt" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$WORKDIR_F/useq0.lrz" 2>/dev/null | grep -q 'lzma+'; then
		log "FAIL  filter/ultra-none-classic"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  filter/ultra-none-classic"
		PASS_OK=$((PASS_OK + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -d -o "$WORKDIR_F/useq.out" "$WORKDIR_F/useq.lrz" >/dev/null 2>&1
	if cmp -s "$WORKDIR_F/seq.txt" "$WORKDIR_F/useq.out"; then
		log "PASS  filter/ultra-roundtrip"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  filter/ultra-roundtrip"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	rm -rf "$WORKDIR_F"
	log "filter: done"
	[[ "$PASS_FAIL" -eq 0 ]]
}

# ----------------------------------------------------------------------------
# Part 5: pre-rzip chunk conversion tests
#
# Every 0.7 chunk header carries a prefilter byte. Forced x86 and arm64 must
# chunk convert and round-trip on data the conversion genuinely transforms
# (random data contains branch opcode bytes), including through the
# stored-block path, encryption and full stdio. Auto selection must refuse
# chunks the probe cannot justify - text, and dedup heavy data where
# conversion would break rzip's long range matches. Without --filter the
# prefilter byte is written as NONE.
# ----------------------------------------------------------------------------
run_chunk_filter_tests() {
	local in lrz
	WORKDIR_C="$(mktemp -d "${TMPDIR:-/tmp}/lrzip-chunk.XXXXXX")"
	log "=== Part 5: chunk filter suite (WORKDIR=$WORKDIR_C) ==="

	# Forced chunk conversion round-trips. Random data exercises real
	# byte transformation plus the stored (incompressible) path under it.
	run_one "chunk/x86/incom_large" incom_large "--filter=x86" file 0
	run_one "chunk/arm64/incom_large" incom_large "--filter=arm64" file 0
	run_one "chunk/x86/zeros_large" zeros_large "--filter=x86" file 0
	run_one "chunk/enc/incom_large" incom_large "--filter=x86" file 1
	run_one "chunk/stdio/incom_large" incom_large "--filter=x86" stdio 0
	run_one "chunk/stdout/incom_large" incom_large "--filter=x86" stdout 0

	# Forced x86 on a large file must record the chunk prefilter in the
	# chunk header prefilter byte and be reported by -i.
	in="$WORKDIR_C/big.bin"
	lrz="$WORKDIR_C/big.lrz"
	dd if=/dev/urandom of="$in" bs=1M count=4 status=none
	"$LRZIP" "${BASE_FLAGS[@]}" --filter=x86 -o "$lrz" "$in" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$lrz" 2>/dev/null | grep -q 'Chunk prefilter:  x86 bcj'; then
		log "PASS  chunk/info-prefilter"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  chunk/info-prefilter"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi
	if "$LRZIP" -i "$lrz" 2>/dev/null | grep -q 'lrzip version: 0.7'; then
		log "PASS  chunk/format-0.7"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  chunk/format-0.7"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	# Auto selection must decline the chunk filter on text (no code); the
	# chunk header still carries LRZ_FILTER_NONE in its prefilter byte.
	in="$WORKDIR_C/text.txt"
	lrz="$WORKDIR_C/text.lrz"
	seq 1 700000 > "$in"
	"$LRZIP" "${BASE_FLAGS[@]}" --filter -o "$lrz" "$in" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$lrz" 2>/dev/null | grep -q 'Chunk prefilter:'; then
		log "FAIL  chunk/auto-declines-text"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  chunk/auto-declines-text"
		PASS_OK=$((PASS_OK + 1))
	fi

	# Dedup protection: three copies of the same binary convert
	# differently at each offset, so the probe must refuse the chunk
	# filter rather than break rzip's long range matches.
	in="$WORKDIR_C/dup.bin"
	lrz="$WORKDIR_C/dup.lrz"
	cat "$LRZIP" "$LRZIP" "$LRZIP" > "$in" 2>/dev/null || \
		{ dd if=/dev/urandom of="$WORKDIR_C/one.bin" bs=1M count=2 status=none; \
		  cat "$WORKDIR_C/one.bin" "$WORKDIR_C/one.bin" "$WORKDIR_C/one.bin" > "$in"; }
	"$LRZIP" "${BASE_FLAGS[@]}" --filter -o "$lrz" "$in" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$lrz" 2>/dev/null | grep -q 'Chunk prefilter:'; then
		log "FAIL  chunk/dedup-protected"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  chunk/dedup-protected"
		PASS_OK=$((PASS_OK + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -d -o "$in.out" "$lrz" >/dev/null 2>&1
	if cmp -s "$in" "$in.out"; then
		log "PASS  chunk/dedup-roundtrip"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  chunk/dedup-roundtrip"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	# Without --filter the prefilter byte is written as NONE: -i must
	# parse the archive and show no chunk prefilter, and it must
	# round-trip.
	"$LRZIP" "${BASE_FLAGS[@]}" -o "$WORKDIR_C/plain.lrz" "$WORKDIR_C/text.txt" >/dev/null 2>&1
	if "$LRZIP" -i -vv "$WORKDIR_C/plain.lrz" 2>/dev/null | grep -q 'Chunk prefilter:'; then
		log "FAIL  chunk/off-no-prefilter"
		PASS_FAIL=$((PASS_FAIL + 1))
	else
		log "PASS  chunk/off-no-prefilter"
		PASS_OK=$((PASS_OK + 1))
	fi
	"$LRZIP" "${BASE_FLAGS[@]}" -d -o "$WORKDIR_C/plain.out" "$WORKDIR_C/plain.lrz" >/dev/null 2>&1
	if cmp -s "$WORKDIR_C/text.txt" "$WORKDIR_C/plain.out"; then
		log "PASS  chunk/off-roundtrip"
		PASS_OK=$((PASS_OK + 1))
	else
		log "FAIL  chunk/off-roundtrip"
		PASS_FAIL=$((PASS_FAIL + 1))
	fi

	rm -rf "$WORKDIR_C"
	log "chunk filter: done"
	[[ "$PASS_FAIL" -eq 0 ]]
}

# ============================================================================
# Main
# ============================================================================

if [[ "${SKIP_GOLD:-0}" != 1 ]]; then
	if ! run_gold_tests; then
		STATUS=1
	fi
else
	log "SKIP  classic gold tests (SKIP_GOLD=1)"
fi

if [[ "${SKIP_ROUNDTRIP:-0}" != 1 ]]; then
	if ! run_roundtrip_tests; then
		STATUS=1
	fi
	if ! run_ultra_tests; then
		STATUS=1
	fi
	if ! run_filter_tests; then
		STATUS=1
	fi
	if ! run_chunk_filter_tests; then
		STATUS=1
	fi
else
	log "SKIP  round-trip suite (SKIP_ROUNDTRIP=1)"
fi

if [[ "$STATUS" -eq 0 ]]; then
	echo "tests/regression.sh: ALL PASSED"
else
	echo "tests/regression.sh: FAILED" >&2
fi
exit "$STATUS"
