kSTEP
==============

- Install dependencies: `./install_deps.py`

- Reproduce bugs: `./reproduce.py <bug_name|all> [--run <buggy|fixed|plot>]`

- Checkout Linux source: `./checkout_linux.py <branch|tag|commit> [<name>] [--tarball]`

- Build Linux and kSTEP: `make linux kstep` (default target is `kstep`)

- Run: `./run.py <name> [--smp <smp>] [--mem_mb <mem_mb>] [--params <key=val>...]`

    - `name`: Name of driver to run, see `kmod/driver.c` for available drivers
    - Example: `./run.py sync_wakeup`

- Debug: `./run.py --debug` in two different terminals starts QEMU and GDB
