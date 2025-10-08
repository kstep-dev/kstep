OUT ?= $(abspath ../rootfs/data)
SRC ?= $(shell pwd)
MODULES := $(filter-out %.mod.c,$(wildcard *.c))

KDIR ?= $(abspath ../linux/current)
ifeq ($(wildcard $(KDIR)),)
override KDIR := /lib/modules/$(shell uname -r)/build
$(warning KDIR not found, using default $(KDIR))
endif

obj-m := $(MODULES:.c=.o)

# Allow the use of private headers
ccflags-y := -I$(KDIR) -Wno-unused-variable -Wno-unused-function

$(MODULES:.c=.ko): $(MODULES) $(wildcard *.h)
	$(MAKE) -C ${KDIR} M=$(SRC) modules
	cp $(MODULES:.c=.ko) $(OUT)

.PHONY: clean
clean:
	$(MAKE) -C ${KDIR} M=$(SRC) clean
