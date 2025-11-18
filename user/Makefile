ROOTFS ?= $(abspath ../data/rootfs)
EXES = $(patsubst %.c,$(ROOTFS)/%,$(wildcard *.c))
TARGETS = $(EXES) $(ROOTFS)/init.sh $(ROOTFS)/init

all: $(TARGETS)

$(ROOTFS)/%: %.c ../kmod/sigcode.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

$(ROOTFS)/init.sh: init.sh
	cp $< $@

.PHONY: clean
clean:
	rm -rf $(TARGETS)
