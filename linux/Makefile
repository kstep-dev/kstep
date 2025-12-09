BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

CURR_LINUX := $(abspath current)
CURR_CONFIG := $(abspath current/.config)
KSTEP_CONFIG := $(abspath config.kstep)

.PHONY: kernel
kernel: config
	cd $(CURR_LINUX) && \
	KBUILD_BUILD_TIMESTAMP='1970-01-01' KBUILD_BUILD_VERSION='1' $(BEAR_CMD) \
	$(MAKE) LOCALVERSION=-kstep WERROR=0

.PHONY: config
config: $(CURR_CONFIG)
$(CURR_CONFIG): $(KSTEP_CONFIG)
	cd $(CURR_LINUX) && \
	./scripts/kconfig/merge_config.sh -n $(KSTEP_CONFIG) && \
	touch $(CURR_CONFIG)

.PHONY: clean
clean:
	cd $(CURR_LINUX) && $(MAKE) mrproper
