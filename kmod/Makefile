OUT ?= $(abspath ../rootfs/data)
SRC ?= $(shell pwd)
KDIR ?= $(abspath ../linux/current)

obj-m := main.o trace.o

# Allow the use of private headers
ccflags-y := -I$(KDIR) -Wno-unused-variable -Wno-unused-function

$(obj-m:.o=.ko): $(obj-m:.o=.c) $(wildcard *.h)
	$(MAKE) -C $(KDIR) M=$(SRC) modules
	cp $(obj-m:.o=.ko) $(OUT)

.PHONY: obj
obj:
	$(MAKE) -C $(KDIR) M=$(SRC) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(KDIR) M=$(SRC) clean
