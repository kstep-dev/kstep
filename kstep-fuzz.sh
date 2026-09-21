#!/usr/bin/env bash
# Build and run the fuzzer: ./kstep-fuzz.sh <bug> [--cores <list>], e.g. ./kstep-fuzz.sh freeze --cores 0-7
exec cargo run --release --manifest-path "$(dirname "$0")/Cargo.toml" -p kstep-fuzz -- "$@"
