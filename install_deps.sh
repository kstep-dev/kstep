#!/bin/env bash

sudo apt update && sudo apt install -y \
    build-essential \
    flex \
    bison \
    bc \
    libncurses-dev \
    libssl-dev \
    libelf-dev \
    bear \
    qemu-kvm \
    gdb \
    cpio

# Install uv
curl -LsSf https://astral.sh/uv/install.sh | sh
