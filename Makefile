MAKEFLAGS += -j$(shell nproc)

ROOTFS_DATA := $(abspath data/rootfs)
ROOTFS_IMG := $(abspath data/rootfs.cpio)

.PHONY: kstep
kstep: $(ROOTFS_IMG)
$(ROOTFS_IMG): user kmod
	mkdir -p $(ROOTFS_DATA)
	cp kmod/build/current/kstep.ko $(ROOTFS_DATA)
	cp user/build/* $(ROOTFS_DATA)
	cd $(ROOTFS_DATA) && (find . -print0 | cpio -o -H newc --verbose --null > $(ROOTFS_IMG))

.PHONY: user
user:
	$(MAKE) -C user

.PHONY: kmod
kmod:
	$(MAKE) -C kmod

.PHONY: linux
linux:
	$(MAKE) -C linux

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -rf $(ROOTFS_DATA)
	rm -f $(ROOTFS_IMG)
