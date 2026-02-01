include ../scripts/common.mk

.PHONY: all
all: build build/rq_btf run

build:
	mkdir -p build

build/%: %.c
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static \
		-I $(LINUX_DIR)/tools/lib -I $(LINUX_DIR)/tools/lib/bpf \
		-o $@ $< $(LINUX_DIR)/tools/lib/bpf/libbpf.a -lelf -lz -lzstd
	
run: build/rq_btf
	./build/rq_btf $(LINUX_DIR)/vmlinux ${KMOD_DIR}

.PHONY: clean
clean:
	rm -rf build
	rm -f ${KMOD_DIR}/rq_fields_gen.h
