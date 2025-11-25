BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)
src ?= $(shell pwd)

kern-src ?= $(realpath $(src)/../linux/current)
kern-ver := $(notdir $(kern-src))
ifeq ($(kern-src),)
    $(error kern-src does not exist)
endif

kmod-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))
kmod-build := $(src)/build/$(kern-ver)

obj-m := kstep.o
kstep-y := $(kmod-srcs:.c=.o)

ccflags-y := \
  -I$(kern-src) \
  -Wno-unused-variable \
  -Wno-unused-function \
  -Wno-declaration-after-statement \
  -std=gnu11

$(kmod-build)/kstep.ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	mkdir -p $(kmod-build)
	touch $(kmod-build)/Makefile
	$(BEAR_CMD) $(MAKE) -C $(kern-src) M=$(kmod-build) srcroot=$(src) src=$(src) modules
	rm -rf $(src)/build/current
	ln -s $(kern-ver) $(src)/build/current

.PHONY: clean
clean:
	rm -rf build
