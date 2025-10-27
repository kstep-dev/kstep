insmod main.ko
busy
# insmod trace.ko func_names=sched_balance_rq
insmod trace.ko func_names=sched_tick
# stat
