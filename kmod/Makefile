OUT ?= $(abspath ../data/rootfs)

src ?= $(shell pwd)
ksrc ?= $(abspath $(src)/../linux/current)

obj-m := schedtest.o
schedtest-y := main.o ksym.o trace.o sched_clock.o

# Allow the use of private headers
ccflags-y := -I$(ksrc) -Wno-unused-variable -Wno-unused-function

schedtest.ko: $(schedtest-y:%.o=%.c) $(wildcard $(src)/*.h)
	$(MAKE) -C $(ksrc) M=$(src) modules
	cp schedtest.ko $(OUT)

.PHONY: obj
obj:
	$(MAKE) -C $(ksrc) M=$(src) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(ksrc) M=$(src) clean
