OUT ?= $(abspath ../rootfs/data/bin)
EXES = $(patsubst %.c,$(OUT)/%,$(wildcard *.c))

all: $(EXES)

$(OUT)/%: %.c ../kmod/sigcode.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf $(EXES)
