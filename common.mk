# Shared build settings for userspace tools.
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR ?= $(PROJECT_ROOT)/../linux

CFLAGS ?= -O2 -g
CPPFLAGS += -I$(PROJECT_ROOT)/include -I$(KERNEL_DIR)/include/uapi
CPPFLAGS += -D__EXPORTED_HEADERS__
# Raw UAPI headers can retain annotations removed by headers_install.
CPPFLAGS += -D__user=
CPPFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -Wall -Wextra
