OUT ?= $(abspath ../rootfs/data)

src ?= $(shell pwd)
ksrc ?= $(abspath $(src)/../linux/current)
obj-m := main.o trace.o

# Allow the use of private headers
ccflags-y := -I$(ksrc) -Wno-unused-variable -Wno-unused-function

$(obj-m:.o=.ko): $(obj-m:.o=.c) $(wildcard $(src)/*.h)
	$(MAKE) -C $(ksrc) M=$(src) modules
	cp $(obj-m:.o=.ko) $(OUT)

.PHONY: obj
obj:
	$(MAKE) -C $(ksrc) M=$(src) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(ksrc) M=$(src) clean
