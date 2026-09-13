#!/bin/bash

set -e
set -x

PACKAGES=(
    build-essential
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

# Install uv
curl -LsSf https://astral.sh/uv/install.sh | sh
