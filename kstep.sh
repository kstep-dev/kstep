#!/usr/bin/env bash
# Build and run the kstep CLI: ./kstep.sh <checkout|build|run|reproduce|viz> [args]
exec cargo run --release --manifest-path "$(dirname "$0")/Cargo.toml" -p kstep -- "$@"
