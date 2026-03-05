# Add -j if not already supplied by user
ifeq ($(findstring -j,$(MAKEFLAGS)),)
  MAKEFLAGS += -j$(shell nproc)
endif

# Add bear if available
BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

# Absolute path to the project directory
PROJ_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

# Resolve LINUX_NAME and LINUX_DIR
LINUX_NAME ?= $(notdir $(realpath $(PROJ_DIR)/linux/current))
ifeq ($(LINUX_NAME),)
    $(error linux/current does not exist or is a broken symlink)
endif
ifeq ($(origin LINUX_NAME),file)
    $(info Using LINUX_NAME=$(LINUX_NAME) (from linux/current symlink))
endif

LINUX_DIR := $(PROJ_DIR)/linux/$(LINUX_NAME)
