.DEFAULT_GOAL := kstep

.PHONY: all
all:
	$(MAKE) linux
	$(MAKE) kstep

# ========= common =========

PROJ_DIR := $(CURDIR)
BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

NAME ?= $(notdir $(realpath $(PROJ_DIR)/build/current))
ifeq ($(NAME),)
    $(error NAME is unset and build/current is missing; run ./checkout.py <version> first, or pass NAME=<version> explicitly)
endif
$(info ======= NAME: $(NAME) =======)

BUILD_DIR := $(PROJ_DIR)/build/$(NAME)

# ========= user =========

.PHONY: user
user: $(BUILD_DIR)/user

$(BUILD_DIR)/user: $(PROJ_DIR)/user/user.c $(PROJ_DIR)/user/user.h
	mkdir -p $(dir $@)
	musl-gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $<

# ========= kmod =========

KMOD_SRC_DIR := $(PROJ_DIR)/kmod
KMOD_OUT_DIR := $(BUILD_DIR)/kmod

.PHONY: kmod
kmod: $(KMOD_OUT_DIR)/kmod.ko

$(KMOD_OUT_DIR)/kmod.ko: $(shell find $(KMOD_SRC_DIR) -type f) $(BUILD_DIR)/kernel
	mkdir -p $(dir $@)
	find $(KMOD_OUT_DIR) -type l -delete
	cp -rs $(KMOD_SRC_DIR)/* $(KMOD_OUT_DIR)
	cd $(BUILD_DIR) && $(BEAR_CMD) $(MAKE) -j$(shell nproc) -C $(LINUX_DIR) M=$(KMOD_OUT_DIR) modules

# ========= kstep =========

ROOTFS_DIR := $(BUILD_DIR)/rootfs

.PHONY: kstep
kstep: $(BUILD_DIR)/rootfs.cpio
$(BUILD_DIR)/rootfs.cpio: $(KMOD_OUT_DIR)/kmod.ko $(BUILD_DIR)/user
	rm -rf $(ROOTFS_DIR)
	mkdir -p $(ROOTFS_DIR)
	cp -p $^ $(ROOTFS_DIR)/
	touch -d @0 $(ROOTFS_DIR) $(ROOTFS_DIR)/*
	cd $(ROOTFS_DIR) && find . | sort | cpio -o --format=newc --reproducible --quiet > $@

# ========= linux =========

LINUX_DIR := $(BUILD_DIR)/linux

ARCH := $(shell uname -m)
ifeq ($(ARCH), x86_64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/x86/boot/bzImage
else ifeq ($(ARCH), aarch64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/arm64/boot/Image
else
	$(error Unsupported architecture: $(ARCH))
endif

.PHONY: linux
linux $(BUILD_DIR)/kernel: $(LINUX_DIR)/.config
	cd $(LINUX_DIR) && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(MAKE) -j$(shell nproc) LOCALVERSION=-$(NAME) WERROR=0 HOSTCFLAGS=-Wno-error
	cp $(LINUX_IMAGE) $(BUILD_DIR)/kernel
	cp $(LINUX_DIR)/vmlinux $(BUILD_DIR)/vmlinux

KSTEP_CONFIG := $(PROJ_DIR)/linux/config.kstep
KSTEP_EXTRA_CONFIG ?=

$(LINUX_DIR)/.config: $(KSTEP_CONFIG) $(KSTEP_CONFIG).$(ARCH) $(KSTEP_EXTRA_CONFIG) | $(LINUX_DIR)/kernel/sched/cov.c
	cd $(LINUX_DIR) && ./scripts/kconfig/merge_config.sh -n $(abspath $^) && touch $@

$(LINUX_DIR)/kernel/sched/cov.c: $(PROJ_DIR)/linux/cov.c $(PROJ_DIR)/linux/Kconfig.kstep $(PROJ_DIR)/linux/Makefile.kstep
	ln -sft $(LINUX_DIR)/kernel/sched/ $^
	echo 'include $$(src)/Makefile.kstep' >> $(LINUX_DIR)/kernel/sched/Makefile
	echo 'source "kernel/sched/Kconfig.kstep"' >> $(LINUX_DIR)/init/Kconfig

# ========= clean =========

.PHONY: clean clean-all clean-kmod clean-user clean-linux
clean: clean-kmod clean-user
clean-all: clean clean-linux

clean-kmod:
	rm -rf $(KMOD_OUT_DIR)

clean-user:
	rm -f $(BUILD_DIR)/user

clean-linux:
	cd $(LINUX_DIR) && $(MAKE) clean
