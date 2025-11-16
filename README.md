kSTEP
==============

- Install dependencies: `./install_deps.sh`

- Download Linux kernel: `./checkout_linux.py [--version <branch/tag/commit>]`

- Build Linux kernel: `./make_linux.py [--clean]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug] [--params <param1=value1>...]` (also triggers `make`)

    - `controller`: Controller name to run, see `kmod/controller.h` for available controllers
    - `trace_funcs`: Function names to trace, see `kmod/trace.c` for available functions
    - Example: `./run_qemu.py --params controller=cd9626e`

- Debug kernel: `./run_gdb.py`
