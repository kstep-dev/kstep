SchedTest
==============

- Install dependencies: `./install_deps.sh`

- Download Linux kernel: `./fetch_linux.py [--versions <version>...] [--tarball]`

- Build Linux kernel: `./make_linux.py [--versions <version>...] [--clean]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug]` (also triggers `make`)

- Debug kernel: `./run_gdb.py`
