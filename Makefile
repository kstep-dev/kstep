include scripts/common.mk

.DEFAULT_GOAL := kstep

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

# ========= linux =========

.PHONY: linux
linux:
	$(MAKE) -C linux LINUX_NAME=$(LINUX_NAME)

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

# ========= clean =========

.PHONY: clean
clean:
	rm -rf $(KMOD_OUT_DIR)
	rm -rf $(USER_OUT_DIR)
	rm -f $(KMOD_SRC_DIR)/compile_commands.json
