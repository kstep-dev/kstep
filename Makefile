# ========= common =========
.DEFAULT_GOAL := kstep

KERNEL ?= $(notdir $(realpath $(CURDIR)/build/current))
ifeq ($(KERNEL),)
    $(error KERNEL unset; run ./checkout.py or pass KERNEL=<name>)
endif
$(info ======= KERNEL: $(KERNEL) =======)

BUILD := $(CURDIR)/build/$(KERNEL)
BEAR := $(if $(shell which bear),bear --append --output $(BUILD)/compile_commands.json --,)

# ========= user =========
USER_OUT := $(CURDIR)/build/user

.PHONY: user
user: $(USER_OUT)

$(USER_OUT): $(CURDIR)/user/user.c $(CURDIR)/user/user.h
	musl-gcc -Wall -Wextra -Wno-unused-parameter -std=c99 -static -o $@ $<

# ========= linux =========
KSTEP_CONFIG := $(CURDIR)/linux/config.kstep
KSTEP_EXTRA_CONFIG ?=

LINUX_BUILT := $(BUILD)/linux/Module.symvers

ARCH := $(shell uname -m)
LINUX_IMAGE-x86_64  := $(BUILD)/linux/arch/x86/boot/bzImage
LINUX_IMAGE-aarch64 := $(BUILD)/linux/arch/arm64/boot/Image
LINUX_IMAGE := $(or $(LINUX_IMAGE-$(ARCH)),$(error Unsupported architecture: $(ARCH)))

$(LINUX_BUILT):
	$(MAKE) linux

.PHONY: linux
linux: $(BUILD)/linux/.config
	cd $(BUILD)/linux && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(MAKE) -j$(shell nproc) LOCALVERSION=-$(KERNEL) WERROR=0 HOSTCFLAGS=-Wno-error
	cp $(LINUX_IMAGE) $(BUILD)/kernel
	cp $(BUILD)/linux/vmlinux $(BUILD)/vmlinux

$(BUILD)/linux/.config: $(KSTEP_CONFIG) $(KSTEP_CONFIG).$(ARCH) $(KSTEP_EXTRA_CONFIG) | $(BUILD)/linux/kernel/sched/cov.c
	cd $(BUILD)/linux && ./scripts/kconfig/merge_config.sh -n $(abspath $^) && touch $@

$(BUILD)/linux/kernel/sched/cov.c: $(CURDIR)/linux/cov.c $(CURDIR)/linux/Kconfig.kstep $(CURDIR)/linux/Makefile.kstep
	ln -sft $(BUILD)/linux/kernel/sched/ $^
	echo 'include $$(src)/Makefile.kstep' >> $(BUILD)/linux/kernel/sched/Makefile
	echo 'source "kernel/sched/Kconfig.kstep"' >> $(BUILD)/linux/init/Kconfig

# ========= kmod =========
KMOD_OUT := $(BUILD)/kmod.ko

.PHONY: kmod
kmod: $(KMOD_OUT)
$(KMOD_OUT): $(shell find $(CURDIR)/kmod -type f) | $(LINUX_BUILT)
	mkdir -p $(BUILD)/kmod
	find $(BUILD)/kmod -type l -delete
	cp -rs $(CURDIR)/kmod/* $(BUILD)/kmod
	cd $(BUILD) && $(BEAR) $(MAKE) -j$(shell nproc) -C $(BUILD)/linux M=$(BUILD)/kmod modules
	cp $(BUILD)/kmod/kmod.ko $@

# ========= kstep =========
ROOTFS_DIR := $(BUILD)/rootfs

.PHONY: kstep
kstep: $(BUILD)/rootfs.cpio
$(BUILD)/rootfs.cpio: $(KMOD_OUT) $(USER_OUT)
	rm -rf $(ROOTFS_DIR)
	mkdir -p $(ROOTFS_DIR)
	cp -p $^ $(ROOTFS_DIR)/
	touch -d @0 $(ROOTFS_DIR) $(ROOTFS_DIR)/*
	cd $(ROOTFS_DIR) && find . | sort | cpio -o --format=newc --reproducible --quiet > $@

# ========= clean =========
.PHONY: clean clean-all
clean:
	rm -rf $(BUILD)/kmod

clean-all: clean
	rm -f $(USER_OUT)
	cd $(BUILD)/linux && $(MAKE) clean
