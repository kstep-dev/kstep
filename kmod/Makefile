OUT ?= $(abspath ../rootfs/data)

src ?= $(shell pwd)
srcroot ?= $(abspath $(src)/../linux/current)
obj-m := main.o trace.o

# Allow the use of private headers
ccflags-y := -I$(srcroot) -Wno-unused-variable -Wno-unused-function

$(obj-m:.o=.ko): $(obj-m:.o=.c) $(wildcard $(src)/*.h)
	$(MAKE) -C $(srcroot) M=$(src) modules
	cp $(obj-m:.o=.ko) $(OUT)

.PHONY: obj
obj:
	$(MAKE) -C $(srcroot) M=$(src) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(srcroot) M=$(src) clean
