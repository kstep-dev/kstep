.PHONY: all
all: build build/init build/task

build:
	mkdir -p build

build/%: %.c ../kmod/user.h utils.h
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf build
