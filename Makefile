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
BUILD_CURR_DIR := $(PROJ_DIR)/build/current

# ========= user =========

USER_SRC_DIR := $(PROJ_DIR)/user
USER_SRC_FILES := $(wildcard $(USER_SRC_DIR)/*)
USER_OUT_DIR := $(BUILD_DIR)/user
USER_OUT_FILES := $(USER_OUT_DIR)/init $(USER_OUT_DIR)/task

.PHONY: user
user: build-current $(USER_OUT_FILES)

$(USER_OUT_DIR)/%: $(USER_SRC_FILES) | $(USER_OUT_DIR)
	gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $(USER_SRC_DIR)/$*.c

$(USER_OUT_DIR):
	mkdir -p $@

# ========= kmod =========

KMOD_SRC_DIR := $(PROJ_DIR)/kmod
KMOD_SRC_FILES := $(shell find $(KMOD_SRC_DIR) -name '*.c' -o -name '*.h')
KMOD_OUT_DIR := $(BUILD_DIR)/kmod
KMOD_OUT_FILE := $(KMOD_OUT_DIR)/kmod.ko

.PHONY: kmod
kmod: build-current $(KMOD_OUT_FILE)

$(KMOD_OUT_FILE): $(KMOD_SRC_FILES) | $(KMOD_OUT_DIR)
	find $(KMOD_OUT_DIR) -type l -delete
	cp -rs $(KMOD_SRC_DIR)/* $(KMOD_OUT_DIR)
	cd $(BUILD_DIR) && $(BEAR_CMD) $(MAKE) -C $(LINUX_DIR) M=$(KMOD_OUT_DIR) modules

$(KMOD_OUT_DIR):
	mkdir -p $@

# ========= kstep =========

ROOTFS_IMG := $(BUILD_DIR)/rootfs.cpio

.PHONY: kstep
kstep: build-current $(ROOTFS_IMG)

$(ROOTFS_IMG): $(KMOD_OUT_FILE) $(USER_OUT_FILES)
	cd $(KMOD_OUT_DIR) && echo kmod.ko | cpio -o --format=newc > $(ROOTFS_IMG)
	cd $(USER_OUT_DIR) && ls | cpio -o --format=newc >> $(ROOTFS_IMG)

# ========= linux =========

ARCH := $(shell uname -m)
ifeq ($(ARCH), x86_64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/x86/boot/bzImage
else ifeq ($(ARCH), aarch64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/arm64/boot/Image
else
	$(error Unsupported architecture: $(ARCH))
endif

.PHONY: linux
linux: build-current linux-config linux-patch
	cd $(LINUX_DIR) && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(MAKE) LOCALVERSION=-$(LINUX_NAME) WERROR=0 HOSTCFLAGS=-Wno-error
	mkdir -p $(BUILD_DIR)
	cp $(LINUX_IMAGE) $(BUILD_DIR)/image
	cp $(LINUX_DIR)/vmlinux $(BUILD_DIR)/vmlinux

.PHONY: build-current
build-current:
	mkdir -p $(dir $(BUILD_CURR_DIR))
	ln -sfn $(BUILD_DIR) $(BUILD_CURR_DIR)

KSTEP_CONFIG := $(PROJ_DIR)/linux/config.kstep
KSTEP_EXTRA_CONFIG ?=

.PHONY: linux-config
linux-config: $(LINUX_DIR)/.config
$(LINUX_DIR)/.config: $(KSTEP_CONFIG) $(KSTEP_CONFIG).$(ARCH) $(KSTEP_EXTRA_CONFIG)
	cd $(LINUX_DIR) && ./scripts/kconfig/merge_config.sh -n $^ && touch $@

.PHONY: linux-patch
linux-patch: $(LINUX_DIR)/kernel/sched/cov.c
$(LINUX_DIR)/kernel/sched/cov.c: $(PROJ_DIR)/linux/cov.c $(PROJ_DIR)/linux/Kconfig.kstep $(PROJ_DIR)/linux/Makefile.kstep
	ln -sft $(LINUX_DIR)/kernel/sched/ $(PROJ_DIR)/linux/cov.c $(PROJ_DIR)/linux/Kconfig.kstep $(PROJ_DIR)/linux/Makefile.kstep
	echo 'include $$(src)/Makefile.kstep' >> $(LINUX_DIR)/kernel/sched/Makefile
	echo 'source "kernel/sched/Kconfig.kstep"' >> $(LINUX_DIR)/init/Kconfig

.PHONY: linux-clean
linux-clean:
	cd $(LINUX_DIR) && $(MAKE) clean

# ========= clean =========

.PHONY: clean
clean:
	rm -rf $(KMOD_OUT_DIR)
	rm -rf $(USER_OUT_DIR)
