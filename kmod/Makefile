OUT ?= $(abspath ../data/rootfs)
src ?= $(shell pwd)

.PHONY: all
all: $(OUT)/kmod.ko

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

$(kmod-name).ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	$(MAKE) -C $(kern-src) M=$(src) modules

$(OUT)/kmod.ko: $(kmod-name).ko
	cp $< $@

.PHONY: clean
clean:
	$(MAKE) -C $(kern-src) M=$(src) clean
