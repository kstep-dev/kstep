.PHONY: all
all: build build/init build/task

build:
	mkdir -p build

build/task: task.c kcov_helpers.c kcov_helpers.h ../kmod/user.h utils.h
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ task.c kcov_helpers.c

build/%: %.c ../kmod/user.h utils.h
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf build
