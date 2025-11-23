ROOTFS ?= $(abspath ../data/rootfs)
TARGETS = $(patsubst %.c,$(ROOTFS)/%,$(wildcard *.c))

all: $(TARGETS)

$(ROOTFS)/%: %.c ../kmod/sigcode.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf $(TARGETS)
