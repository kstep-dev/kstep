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
    gcc-x86-64-linux-gnu
    gcc-aarch64-linux-gnu
    qemu-system-x86
    qemu-system-arm
    cpio           # rootfs
)

# Install apt packages
sudo apt update
sudo apt install -y "${PACKAGES[@]}"

# Install uv
curl -LsSf https://astral.sh/uv/install.sh | sh
