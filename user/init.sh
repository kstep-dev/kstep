# Available parameters:
# - controller_name: Controller name to run, see `kmod/controller.h` for available controllers
# - trace_funcs: Function names to trace, see `kmod/trace.c` for available functions
# - json: Output in JSON format
insmod schedtest.ko trace_funcs=sched_tick controller_name=aa3ee4f
# insmod schedtest.ko trace_funcs=sched_tick
busy
# stat
