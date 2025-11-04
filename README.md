SchedTest
==============

- Install dependencies: `./install_deps.sh`

- Download Linux kernel: `./fetch_linux.py [--versions <version>...] [--tarball]`

- Build Linux kernel: `./make_linux.py [--versions <version>...] [--clean]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug] [--params <param1=value1>...]` (also triggers `make`)

    - `controller`: Controller name to run, see `kmod/controller.h` for available controllers
    - `trace_funcs`: Function names to trace, see `kmod/trace.c` for available functions
    - `json`: Output in JSON format
    - Example: `./run_qemu.py --params controller=cd9626e`

- Debug kernel: `./run_gdb.py`
