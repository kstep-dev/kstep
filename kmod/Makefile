OUT ?= $(abspath ../rootfs/data)
SRC ?= $(shell pwd)
MODULES := $(filter-out %.mod.c,$(wildcard *.c))
KERNELDIR ?= $(abspath ../linux/current)
ifeq ($(wildcard $(KERNELDIR)),)
override KERNELDIR := /lib/modules/$(shell uname -r)/build
endif
$(info KERNELDIR: $(KERNELDIR))

obj-m := $(MODULES:.c=.o)

# Allow the use of private headers
ccflags-y := -I$(KERNELDIR) -Wno-unused-variable -Wno-unused-function

$(MODULES:.c=.ko): $(MODULES) $(wildcard *.h)
	$(MAKE) -C ${KERNELDIR} M=$(SRC) modules
	cp $(SRC)/*.ko $(OUT)

.PHONY: clean
clean:
	$(MAKE) -C ${KERNELDIR} M=$(SRC) clean
