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
    musl-tools     # musl-gcc for static user binaries
    cpio           # rootfs
    bear           # clangd completion
    qemu-kvm       # run virtual machine
    gdb            # optional kernel debugging
)

# Install apt packages
sudo apt update
sudo apt install -y "${PACKAGES[@]}"

# Install uv
curl -LsSf https://astral.sh/uv/install.sh | sh
