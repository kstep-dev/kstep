OUT ?= $(abspath ../data/rootfs)
src ?= $(shell pwd)

kern-src ?= $(realpath $(src)/../linux/current)
kern-ver := $(notdir $(kern-src))
ifeq ($(kern-src),)
    $(error kern-src does not exist)
endif

kmod-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))
kmod-name := kstep-$(kern-ver)

obj-m := $(kmod-name).o
$(kmod-name)-y := $(kmod-srcs:.c=.o)

ccflags-y := \
  -I$(kern-src) \
  -Wno-unused-variable \
  -Wno-unused-function \
  -Wno-declaration-after-statement \
  -std=gnu11

.PHONY: all
all: $(kmod-name).ko
	cp $(kmod-name).ko $(OUT)/kmod.ko

$(kmod-name).ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	$(MAKE) -C $(kern-src) M=$(src) modules

.PHONY: clean
clean:
	$(MAKE) -C $(kern-src) M=$(src) clean
	rm -f $(OUT)/kmod.ko
