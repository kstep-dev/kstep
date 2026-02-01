include scripts/common.mk

ROOTFS_DIR := $(PROJ_DIR)/data/rootfs
ROOTFS_IMG := $(ROOTFS_DIR)/$(LINUX_NAME).cpio

.PHONY: kstep
kstep: user autotrace kmod
	cd $(PROJ_DIR)/kmod/build/$(LINUX_NAME) && echo kstep.ko | cpio -o --format=newc > $(ROOTFS_IMG)
	cd $(PROJ_DIR)/user/build && ls | cpio -o --format=newc >> $(ROOTFS_IMG)

.PHONY: user
user:
	$(MAKE) -C user

.PHONY: kmod
kmod:
	$(MAKE) -C kmod LINUX_DIR=$(LINUX_DIR)

.PHONY: linux
linux:
	$(MAKE) -C linux LINUX_DIR=$(LINUX_DIR)

.PHONY: autotrace
autotrace:
	cd $(LINUX_DIR)/tools/lib/bpf && $(MAKE)
	cd $(LINUX_DIR)/tools/bpf/bpftool && \
		sed -i 's/LIBS = $$(LIBBPF) -lelf -lz/LIBS = $$(LIBBPF) -lelf -lz -lzstd/' Makefile && \
		$(MAKE) EXTRA_LDFLAGS="-static"
	$(MAKE) -C autotrace

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	$(MAKE) -C autotrace clean
	rm -rf $(ROOTFS_DIR)/*.cpio
