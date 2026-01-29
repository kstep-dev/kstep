src ?= $(shell pwd)
include $(src)/../scripts/common.mk

kmod-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))
kmod-build := $(src)/build/$(LINUX_NAME)

obj-m := kstep.o
kstep-y := $(kmod-srcs:.c=.o)
ccflags-y := -I$(LINUX_DIR) -std=gnu11 -Werror -Wno-declaration-after-statement

.PHONY: all
all: $(kmod-build)/kstep.ko
$(kmod-build)/kstep.ko: $(kmod-srcs) $(wildcard $(src)/*.h)
	mkdir -p $(kmod-build)
	ln -sf $(src)/* $(kmod-build) 
	$(BEAR_CMD) $(MAKE) -C $(LINUX_DIR) M=$(kmod-build) src=$(src) modules

.PHONY: clean
clean:
	rm -rf build
	rm -f compile_commands.json
