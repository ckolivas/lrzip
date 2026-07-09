#!/bin/bash
# Compatibility wrapper — round-trip is part of tests/regression.sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export SKIP_GOLD=1
exec "$SCRIPT_DIR/regression.sh" "$@"
