# Add -j if not already supplied by user
ifeq ($(findstring -j,$(MAKEFLAGS)),)
  MAKEFLAGS += -j$(shell nproc)
endif

# Add bear if available
BEAR_CMD := $(if $(shell which bear),bear --append --output compile_commands.json --,)

# Absolute path to the project directory
PROJ_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)

# Resolve LINUX_DIR
LINUX_DIR ?= linux/current
LINUX_DIR_ABS := $(if $(filter /%,$(LINUX_DIR)),$(LINUX_DIR),$(PROJ_DIR)/$(LINUX_DIR))
LINUX_DIR_REAL := $(realpath $(LINUX_DIR_ABS))
ifeq ($(LINUX_DIR_REAL),)
    $(error $(LINUX_DIR_ABS) does not exist)
endif
override LINUX_DIR := $(LINUX_DIR_REAL)

LINUX_NAME := $(notdir $(LINUX_DIR))

# Absolute path to the kmod directory
KMOD_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../kmod)
