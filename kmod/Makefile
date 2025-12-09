BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)
src ?= $(shell pwd)

kern-src ?= $(realpath $(src)/../linux/current)
ifeq ($(kern-src),)
    $(error kern-src does not exist)
endif

kmod-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))
kmod-build := $(src)/build/$(notdir $(kern-src))

obj-m := kstep.o
kstep-y := $(kmod-srcs:.c=.o)
ccflags-y := -I$(kern-src) -std=gnu11

.PHONY: all
all: $(kmod-build)/kstep.ko
	rm -rf $(src)/build/current
	ln -s $(kmod-build) $(src)/build/current

$(kmod-build)/kstep.ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	mkdir -p $(kmod-build)
	ln -sf $(src)/* $(kmod-build) 
	$(BEAR_CMD) $(MAKE) -C $(kern-src) M=$(kmod-build) src=$(src) modules

.PHONY: clean
clean:
	rm -rf build
