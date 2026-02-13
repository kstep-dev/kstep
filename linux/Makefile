include ../scripts/common.mk

LINUX_CONFIG := $(LINUX_DIR)/.config
KSTEP_CONFIG := $(realpath config.kstep)
LINUX_BUILD_DIR := $(abspath build)

ARCH := $(shell uname -m)
ifeq ($(ARCH), x86_64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/x86/boot/bzImage
else ifeq ($(ARCH), aarch64)
	LINUX_IMAGE := $(LINUX_DIR)/arch/arm64/boot/Image
else
	$(error Unsupported architecture: $(ARCH))
endif

.PHONY: kernel
kernel: config patch
	cd $(LINUX_DIR) && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(BEAR_CMD) $(MAKE) LOCALVERSION=-$(LINUX_NAME) WERROR=0 HOSTCFLAGS=-Wno-error
	mkdir -p $(LINUX_BUILD_DIR) && cp $(LINUX_IMAGE) $(LINUX_BUILD_DIR)/$(LINUX_NAME)

.PHONY: config
config: $(LINUX_CONFIG)
$(LINUX_CONFIG): $(KSTEP_CONFIG)
	cd $(LINUX_DIR) && \
	./scripts/kconfig/merge_config.sh -n $(KSTEP_CONFIG) && \
	touch $(LINUX_CONFIG)

COV_SRC := $(abspath cov.c)
COV_DST := $(LINUX_DIR)/kernel/sched/cov.c
COV_MAKEFILE := $(LINUX_DIR)/kernel/sched/Makefile
.PHONY: patch
patch: $(COV_DST)
$(COV_DST): $(COV_SRC)
	ln -sf $(COV_SRC) $(COV_DST)
	echo 'obj-y += cov.o' >> $(COV_MAKEFILE)
	echo 'ccflags-y += -fsanitize-coverage=trace-pc' >> $(COV_MAKEFILE)
	echo 'CFLAGS_REMOVE_cov.o := -fsanitize-coverage=trace-pc' >> $(COV_MAKEFILE)

.PHONY: clean
clean:
	cd $(LINUX_DIR) && $(MAKE) clean
