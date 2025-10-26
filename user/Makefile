ROOTFS ?= $(abspath ../rootfs/data)
EXES = $(patsubst %.c,$(ROOTFS)/bin/%,$(wildcard *.c))
TARGETS = $(EXES) $(ROOTFS)/init.sh

all: $(TARGETS)

$(ROOTFS)/bin/%: %.c ../kmod/sigcode.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

$(ROOTFS)/init.sh: init.sh
	cp $< $@

.PHONY: clean
clean:
	rm -rf $(TARGETS)
