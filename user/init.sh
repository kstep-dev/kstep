insmod main.ko
busy
insmod trace.ko func_names=sched_balance_rq,sched_tick
stat
