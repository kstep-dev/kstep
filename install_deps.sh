#!/bin/bash

set -e
set -x

PACKAGES=(
    build-essential
    wget
    libclang-dev
    flex
    bison
    bc
    libncurses-dev
    libssl-dev
    libelf-dev
    qemu-system-$([ "$(uname -m)" = x86_64 ] && echo x86 || echo arm)
)

# Install apt packages
sudo apt update
sudo apt install -y "${PACKAGES[@]}"

# Install Rust
command -v cargo >/dev/null || curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
# Install uv
command -v uv >/dev/null || curl -LsSf https://astral.sh/uv/install.sh | sh
