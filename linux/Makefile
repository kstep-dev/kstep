include ../scripts/common.mk

LINUX_CONFIG := $(LINUX_DIR)/.config
KSTEP_CONFIG := $(realpath config.kstep)
IMAGE_DIR := $(realpath build)

.PHONY: kernel
kernel: config
	cd $(LINUX_DIR) && KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(BEAR_CMD) $(MAKE) LOCALVERSION=-kstep-$(LINUX_NAME) WERROR=0 HOSTCFLAGS=-Wno-error
	mkdir -p $(IMAGE_DIR) && cp $(LINUX_DIR)/$(shell make -C $(LINUX_DIR) -s image_name) $(IMAGE_DIR)/$(LINUX_NAME)

.PHONY: config
config: $(LINUX_CONFIG)
$(LINUX_CONFIG): $(KSTEP_CONFIG)
	cd $(LINUX_DIR) && ./scripts/kconfig/merge_config.sh -n $(KSTEP_CONFIG)
	touch $(LINUX_CONFIG)

.PHONY: clean
clean:
	cd $(LINUX_DIR) && $(MAKE) clean
