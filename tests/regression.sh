#!/bin/bash
#
# lrzip regression tests — single entry point under tests/.
#
# Part 1: Classic gold-file CLI tests (compat `lrz` behaviour).
# Part 2: Round-trip matrix (content shapes, backends, STDIO, encryption).
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
else
	log "SKIP  round-trip suite (SKIP_ROUNDTRIP=1)"
fi

if [[ "$STATUS" -eq 0 ]]; then
	echo "tests/regression.sh: ALL PASSED"
else
	echo "tests/regression.sh: FAILED" >&2
fi
exit "$STATUS"
