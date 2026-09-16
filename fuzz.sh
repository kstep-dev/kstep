#!/usr/bin/env bash
# Build the fuzzer and run it: ./fuzz.sh <bug> [--cores <list>], e.g. ./fuzz.sh freeze --cores 0-7
set -euo pipefail
exec cargo run --release --quiet --manifest-path "$(dirname "$0")/fuzzer/Cargo.toml" -- "$@"
