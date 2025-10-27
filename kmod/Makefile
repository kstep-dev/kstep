OUT ?= $(abspath ../data/rootfs)

src ?= $(shell pwd)
ksrc ?= $(abspath $(src)/../linux/current)

schedtest-srcs := $(notdir $(filter-out %.mod.c,$(wildcard $(src)/*.c)))

obj-m := schedtest.o
schedtest-y := $(schedtest-srcs:.c=.o)

# Allow the use of private headers
ccflags-y := -I$(ksrc) -Wno-unused-variable -Wno-unused-function

schedtest.ko: $(schedtest-srcs) $(wildcard $(src)/*.h)
	$(MAKE) -C $(ksrc) M=$(src) modules
	cp schedtest.ko $(OUT)

.PHONY: obj
obj:
	$(MAKE) -C $(ksrc) M=$(src) $(obj-m)

.PHONY: clean
clean:
	$(MAKE) -C $(ksrc) M=$(src) clean
