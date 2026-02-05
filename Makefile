include scripts/common.mk

ROOTFS_DIR := $(PROJ_DIR)/data/rootfs
ROOTFS_IMG := $(ROOTFS_DIR)/$(LINUX_NAME).cpio

.PHONY: kstep
kstep: user kmod
	cd $(PROJ_DIR)/kmod/build/$(LINUX_NAME) && echo kmod.ko | cpio -o --format=newc > $(ROOTFS_IMG)
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

.PHONY: clean
clean:
	$(MAKE) -C user clean
	$(MAKE) -C kmod clean
	rm -rf $(ROOTFS_DIR)/*.cpio
