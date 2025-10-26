BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

ROOTFS_DATA := $(abspath data/rootfs)
ROOTFS_IMG := $(abspath data/rootfs.ext4)
ROOTFS_MOUNT := $(abspath data/mount)

.PHONY: all
all: user kmod $(ROOTFS_IMG)

# Build the userspace programs
.PHONY: user
user:
	$(BEAR_CMD) $(MAKE) -C user

# Build the kernel module
.PHONY: kmod
kmod:
	$(BEAR_CMD) $(MAKE) -C kmod

# Build the root filesystem
$(ROOTFS_IMG): user kmod $(shell find $(ROOTFS_DATA) -type f)
	dd if=/dev/zero of=$(ROOTFS_IMG) bs=1M count=64
	mkfs.ext4 -F $(ROOTFS_IMG)
	sudo mount -o loop $(ROOTFS_IMG) $(ROOTFS_MOUNT)
	sudo cp -r $(ROOTFS_DATA)/* $(ROOTFS_MOUNT)/
	sudo umount $(ROOTFS_MOUNT)

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -f $(ROOTFS_IMG)
