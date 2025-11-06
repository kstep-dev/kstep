OUT ?= $(abspath ../data/rootfs)
src ?= $(shell pwd)

kern-src ?= $(realpath $(src)/../linux/current)
kern-ver := $(patsubst linux-%,%,$(notdir $(kern-src)))

kmod-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))
kmod-name := kstep-$(kern-ver)

obj-m := $(kmod-name).o
$(kmod-name)-y := $(kmod-srcs:.c=.o)

# Allow the use of private headers
ccflags-y := -I$(kern-src) -Wno-unused-variable -Wno-unused-function

$(kmod-name).ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	$(MAKE) -C $(kern-src) M=$(src) modules
	cp $(kmod-name).ko $(OUT)/kmod.ko

.PHONY: obj
obj:
	$(MAKE) -C $(kern-src) M=$(src) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(kern-src) M=$(src) clean
