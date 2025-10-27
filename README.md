SchedTest
==============

- Install dependencies: `./install_deps.sh`

- Download Linux kernel: `./fetch_linux.py [--versions <version>...] [--tarball]`

- Build Linux kernel: `./make_linux.py [--versions <version>...] [--clean]`

- Prepare root filesystem: `make`

- Run kernel: `./run_qemu.py [--debug] [--params <param1=value1>...]` (also triggers `make`)

    - `trace_funcs`: Function names to trace, see `kmod/trace.c` for available functions
    - `controller_name`: Controller name to run, see `kmod/controller.h` for available controllers
    - `json`: Output in JSON format
    - Example: `./run_qemu.py --params trace_funcs=sched_tick controller_name=noop json`
    - Example: `./run_qemu.py --params trace_funcs=sched_tick controller_name=cd9626e`

- Debug kernel: `./run_gdb.py`
