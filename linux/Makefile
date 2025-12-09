BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

LINUX_DIR := $(realpath current)
ifeq ($(LINUX_DIR),)
    $(error linux/current does not exist)
endif
LINUX_CONFIG := $(LINUX_DIR)/.config
KSTEP_CONFIG := $(realpath config.kstep)

.PHONY: kernel
kernel: config
	cd $(LINUX_DIR) && \
	KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(BEAR_CMD) \
	$(MAKE) LOCALVERSION=-kstep WERROR=0

.PHONY: config
config: $(LINUX_CONFIG)
$(LINUX_CONFIG): $(KSTEP_CONFIG)
	cd $(LINUX_DIR) && \
	./scripts/kconfig/merge_config.sh -n $(KSTEP_CONFIG) && \
	touch $(LINUX_CONFIG)

.PHONY: clean
clean:
	cd $(LINUX_DIR) && $(MAKE) mrproper
