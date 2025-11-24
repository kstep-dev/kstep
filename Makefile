MAKEFLAGS += -j$(shell nproc)

BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

ROOTFS_DATA := $(abspath data/rootfs)
ROOTFS_IMG := $(abspath data/rootfs.cpio)

.PHONY: all
all: $(ROOTFS_IMG)

# Build the userspace programs
.PHONY: user
user:
	$(MAKE) -C user

# Build the kernel module
.PHONY: kmod
kmod:
	$(BEAR_CMD) $(MAKE) -C kmod

# Build the root filesystem
$(ROOTFS_IMG): user kmod $(shell find $(ROOTFS_DATA) -type f)
	cd $(ROOTFS_DATA) && (find . -print0 | cpio -o -H newc --verbose --null > $(ROOTFS_IMG))

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -f $(ROOTFS_IMG)
