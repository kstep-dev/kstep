.PHONY: all
all: build build/task build/busy

build:
	mkdir -p build

build/%: %.c ../kmod/user.h utils.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf build
