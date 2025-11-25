TARGETS = $(patsubst %.c,%,$(wildcard *.c))

.PHONY: all
all: $(TARGETS)

%: %.c ../kmod/sigcode.h
	gcc -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -std=c99 -static -o $@ $<

.PHONY: clean
clean:
	rm -rf $(TARGETS)
