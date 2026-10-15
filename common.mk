# Shared build settings for userspace tools.
PROJECT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD_ROOT := $(PROJECT_ROOT)/build

CFLAGS ?= -O2 -g
CPPFLAGS += -I$(PROJECT_ROOT)/include
CPPFLAGS += $(if $(KERNEL_HEADERS),-I$(KERNEL_HEADERS))
CPPFLAGS += -D__EXPORTED_HEADERS__
# Raw UAPI headers can retain annotations removed by headers_install.
CPPFLAGS += -D__user=
CPPFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -Wall -Wextra

# Use the compiler's default headers unless an explicit UAPI directory is given.
# Check the definitions needed by the cdev and DMABUF helpers.
ifneq ($(MAKECMDGOALS),clean)
VFIO_UAPI_OK := $(shell printf '%s\n' \
	'struct vfio_device_bind_iommufd bind;' \
	'struct vfio_device_feature_dma_buf buf;' \
	'struct vfio_region_dma_range range;' \
	'unsigned int api = VFIO_DEVICE_BIND_IOMMUFD | VFIO_DEVICE_FEATURE | VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF;' \
	| $(CC) $(CPPFLAGS) -include linux/vfio.h -x c -fsyntax-only - \
	>/dev/null 2>&1 && echo yes)

ifneq ($(VFIO_UAPI_OK),yes)
$(error Required VFIO UAPI unavailable; pass KERNEL_HEADERS=/absolute/path/to/UAPI/headers)
endif
endif
