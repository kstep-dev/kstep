include scripts/common.mk

ROOTFS_DIR := $(PROJ_DIR)/data/rootfs
ROOTFS_DATA := $(ROOTFS_DIR)/$(LINUX_NAME)
ROOTFS_IMG := $(ROOTFS_DIR)/$(LINUX_NAME).cpio

.PHONY: kstep
kstep: $(ROOTFS_IMG)
$(ROOTFS_IMG): user kmod
	mkdir -p $(ROOTFS_DATA)
	cp $(PROJ_DIR)/kmod/build/$(LINUX_NAME)/kstep.ko $(ROOTFS_DATA)
	cp $(PROJ_DIR)/user/build/* $(ROOTFS_DATA)
	cd $(ROOTFS_DATA) && (find . -print0 | cpio -o -H newc --verbose --null > $(ROOTFS_IMG))

.PHONY: user
user:
	$(MAKE) -C user

.PHONY: kmod
kmod:
	$(MAKE) -C kmod LINUX_DIR=$(LINUX_DIR)

.PHONY: linux
linux:
	$(MAKE) -C linux LINUX_DIR=$(LINUX_DIR)

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -rf $(ROOTFS_DIR)
