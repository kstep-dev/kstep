OUT ?= $(abspath ../rootfs/data)
SRC ?= $(shell pwd)
KERNELDIR ?= $(abspath ../linux/current)
MODULES := $(filter-out %.mod.c,$(wildcard *.c))

obj-m := $(MODULES:.c=.o)

# Allow the use of private headers
ccflags-y := -I$(KERNELDIR) -Wno-unused-variable -Wno-unused-function

$(MODULES:.c=.ko): $(MODULES) $(wildcard *.h)
	$(MAKE) -C ${KERNELDIR} M=$(SRC) modules
	cp $(SRC)/*.ko $(OUT)

.PHONY: clean
clean:
	$(MAKE) -C ${KERNELDIR} M=$(SRC) clean
