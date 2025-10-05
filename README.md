SchedTest
==============

- Download Linux kernel: `./clone_linux.py`

- Install dependencies: `./install_deps.sh`

- Build Linux kernel: `./make_linux.py [--uml] [--clean]`

- Prepare root filesystem: `make` (triggered by `./run_*.py` as well)

- Run kernel: `./run_qemu.py [--debug]` or `./run_uml.py`
