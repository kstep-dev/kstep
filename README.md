SchedTest
==============

- Download Linux kernel: `./fetch_linux.py [--versions <version>...] [--tarball]`

- Install dependencies: `./install_deps.sh`

- Build Linux kernel: `./make_linux.py [--clean]`

- Prepare root filesystem: `make` (also triggered by `./run_*.py`)

- Run kernel: `./run_qemu.py [--debug]`

- Debug kernel: `./run_gdb.py`
