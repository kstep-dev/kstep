.DEFAULT_GOAL := kstep

# ========= common =========

PROJ_DIR := $(CURDIR)
MAKEFLAGS := $(MAKEFLAGS) $(if $(findstring -j,$(MAKEFLAGS)),,-j$(shell nproc))
BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

LINUX_NAME ?= $(notdir $(realpath $(PROJ_DIR)/linux/current))
ifeq ($(LINUX_NAME),)
    $(error linux/current does not exist or is a broken symlink)
endif
$(info ======= LINUX_NAME: $(LINUX_NAME) =======)

LINUX_DIR := $(PROJ_DIR)/linux/$(LINUX_NAME)
BUILD_DIR := $(PROJ_DIR)/build/$(LINUX_NAME)

# ========= user =========

USER_SRC_DIR := $(PROJ_DIR)/user
USER_SRC_FILES := $(wildcard $(USER_SRC_DIR)/*)
USER_OUT_DIR := $(BUILD_DIR)/user
USER_OUT_FILES := $(USER_OUT_DIR)/init $(USER_OUT_DIR)/task

.PHONY: user
user: $(USER_OUT_FILES)

$(USER_OUT_DIR)/%: $(USER_SRC_FILES) | $(USER_OUT_DIR)
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $(USER_SRC_DIR)/$*.c

$(USER_OUT_DIR):
	mkdir -p $@

# ========= kmod =========

KMOD_SRC_DIR := $(PROJ_DIR)/kmod
KMOD_SRC_FILES := $(wildcard $(KMOD_SRC_DIR)/*.c $(KMOD_SRC_DIR)/*.h $(KMOD_SRC_DIR)/drivers/*.c)
KMOD_OUT_DIR := $(BUILD_DIR)/kmod
KMOD_OUT_FILE := $(KMOD_OUT_DIR)/kmod.ko

.PHONY: kmod
kmod: $(KMOD_OUT_FILE)

$(KMOD_OUT_FILE): $(KMOD_SRC_FILES) | $(KMOD_OUT_DIR)
	ln -sf $(KMOD_SRC_DIR)/*.c $(KMOD_SRC_DIR)/*.h $(KMOD_SRC_DIR)/Kbuild $(KMOD_OUT_DIR)
	mkdir -p $(KMOD_OUT_DIR)/drivers
	ln -sf $(KMOD_SRC_DIR)/drivers/*.c $(KMOD_OUT_DIR)/drivers
	cd $(KMOD_OUT_DIR) && $(BEAR_CMD) $(MAKE) -C $(LINUX_DIR) M=$(KMOD_OUT_DIR) modules
	ln -sf $(KMOD_OUT_DIR)/compile_commands.json $(KMOD_SRC_DIR)/compile_commands.json

$(KMOD_OUT_DIR):
	mkdir -p $@

# ========= kstep =========

ROOTFS_IMG := $(BUILD_DIR)/rootfs.cpio

.PHONY: kstep
kstep: $(ROOTFS_IMG)

$(ROOTFS_IMG): $(KMOD_OUT_FILE) $(USER_OUT_FILES)
	cd $(KMOD_OUT_DIR) && echo kmod.ko | cpio -o --format=newc > $(ROOTFS_IMG)
	cd $(USER_OUT_DIR) && ls | cpio -o --format=newc >> $(ROOTFS_IMG)

# ========= linux =========

LINUX_CONFIG := $(LINUX_DIR)/.config
KSTEP_CONFIG := $(PROJ_DIR)/linux/config.kstep

ARCH := $(shell uname -m)
ifeq ($(ARCH), x86_64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/x86/boot/bzImage
else ifeq ($(ARCH), aarch64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/arm64/boot/Image
else
	$(error Unsupported architecture: $(ARCH))
endif

.PHONY: linux
linux: linux-config linux-patch
	cd $(LINUX_DIR) && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(MAKE) LOCALVERSION=-$(LINUX_NAME) WERROR=0 HOSTCFLAGS=-Wno-error
	mkdir -p $(BUILD_DIR)
	cp $(LINUX_IMAGE) $(BUILD_DIR)/image
	cp $(LINUX_DIR)/vmlinux $(BUILD_DIR)/vmlinux

.PHONY: linux-config
linux-config: $(LINUX_CONFIG)
$(LINUX_CONFIG): $(KSTEP_CONFIG) $(KSTEP_CONFIG).$(ARCH)
	cd $(LINUX_DIR) && ./scripts/kconfig/merge_config.sh -n $(KSTEP_CONFIG) $(KSTEP_CONFIG).$(ARCH)
	touch $(LINUX_CONFIG)

.PHONY: linux-patch
linux-patch: $(LINUX_DIR)/kernel/cov.c
$(LINUX_DIR)/kernel/cov.c: $(PROJ_DIR)/linux/cov.c
	ln -sf $< $@
	echo 'obj-y += cov.o' >> $(LINUX_DIR)/kernel/Makefile
	echo 'ccflags-y += -fsanitize-coverage=trace-pc' >> $(LINUX_DIR)/kernel/sched/Makefile

.PHONY: linux-clean
linux-clean:
	cd $(LINUX_DIR) && $(MAKE) clean

# ========= clean =========

.PHONY: clean
clean:
	rm -rf $(KMOD_OUT_DIR)
	rm -rf $(USER_OUT_DIR)
	rm -f $(KMOD_SRC_DIR)/compile_commands.json
