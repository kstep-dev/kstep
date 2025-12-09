MAKEFLAGS += -j$(shell nproc)

ROOTFS_DATA := $(abspath data/rootfs)
ROOTFS_IMG := $(abspath data/rootfs.cpio)

.PHONY: rootfs
rootfs: $(ROOTFS_IMG)

# Build the userspace programs
.PHONY: user
user:
	$(MAKE) -C user

# Build the kernel module
.PHONY: kmod
kmod:
	$(MAKE) -C kmod

# Build the root filesystem
$(ROOTFS_IMG): user kmod
	mkdir -p $(ROOTFS_DATA)
	cp kmod/build/current/kstep.ko $(ROOTFS_DATA)
	cp user/build/* $(ROOTFS_DATA)
	cd $(ROOTFS_DATA) && (find . -print0 | cpio -o -H newc --verbose --null > $(ROOTFS_IMG))

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -rf $(ROOTFS_DATA)
	rm -f $(ROOTFS_IMG)
