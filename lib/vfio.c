#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "vfio.h"

int vfio_open_cdev(const char *path, int *iommufd)
{
	struct vfio_device_bind_iommufd bind = {
		.argsz = sizeof(bind),
	};
	int fd, iommu, saved_errno;

	*iommufd = -1;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open VFIO cdev %s: %s\n",
			path, strerror(errno));
		return -1;
	}

	iommu = open("/dev/iommu", O_RDWR | O_CLOEXEC);
	if (iommu < 0) {
		fprintf(stderr, "Cannot open /dev/iommu: %s\n", strerror(errno));
		goto close_device;
	}

	bind.iommufd = iommu;
	if (ioctl(fd, VFIO_DEVICE_BIND_IOMMUFD, &bind) < 0) {
		fprintf(stderr, "Cannot bind VFIO cdev %s to /dev/iommu: %s\n",
			path, strerror(errno));
		saved_errno = errno;
		close(fd);
		close(iommu);
		errno = saved_errno;
		return -1;
	}

	*iommufd = iommu;
	return fd;

close_device:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

int vfio_open_traditional(int groupnr, const char *rid_str)
{
	/* Create a new container, add group to it, open device, read
	 * resource, reset, etc.  Based on the example code in
	 * Documentation/driver-api/vfio.rst
	 */
	int container = open("/dev/vfio/vfio", O_RDWR);

	int r = ioctl(container, VFIO_GET_API_VERSION);

	if (r != VFIO_API_VERSION) {
		/* Unknown API version */
		fprintf(stderr, "Unknown API ver %d\n", r);
		return -1;
	}

	if (ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) != 1) {
		fprintf(stderr, "Doesn't support type 1\n");
		return -1;
	}

	char devpath[PATH_MAX];

	snprintf(devpath, PATH_MAX - 1, "/dev/vfio/%d", groupnr);
	/* Open the group */
	int group = open(devpath, O_RDWR);

	if (group < 0) {
		fprintf(stderr, "Can't open VFIO device (group %d)\n", groupnr);
		return -1;
	}

	/* Test the group is viable and available */
	struct vfio_group_status group_status = { .argsz = sizeof(
							  group_status) };

	if (ioctl(group, VFIO_GROUP_GET_STATUS, &group_status)) {
		perror("Can't get group status");
		return -1;
	}

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		/* Group is not viable (ie, not all devices bound for vfio) */
		fprintf(stderr, "Group %d is not viable!\n", groupnr);
		return -1;
	}

	/* Add the group to the container */
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container)) {
		perror("Can't add group to container");
		return -1;
	}

	/* Enable the IOMMU model we want */
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
		perror("Can't select T1");
		return -1;
	}

	/* Get additional IOMMU info */
	struct vfio_iommu_type1_info iommu_info = { .argsz = sizeof(
							    iommu_info) };

	if (ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info)) {
		perror("Can't get VFIO info");
		return -1;
	}

	/* Get a file descriptor for the device */
	int device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, rid_str);

	if (device < 0) {
		perror("Can't get device fd");
		return -1;
	}
	close(container);
	close(group);

	return device;
}

int vfio_get_device_info(int fd, struct vfio_device_info *info)
{
	memset(info, 0, sizeof(*info));
	info->argsz = sizeof(*info);
	if (ioctl(fd, VFIO_DEVICE_GET_INFO, info) < 0) {
		fprintf(stderr, "VFIO_DEVICE_GET_INFO failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

int vfio_export_region_dmabuf(int fd, const struct vfio_region *region)
{
	struct {
		struct vfio_device_feature feature;
		struct vfio_device_feature_dma_buf dma_buf;
		struct vfio_region_dma_range range;
	} request = {
		.feature = {
			.argsz = sizeof(request),
			.flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF,
		},
		.dma_buf = {
			.region_index = region->index,
			.open_flags = O_RDWR | O_CLOEXEC,
			.flags = 0,
			.nr_ranges = 1,
		},
		.range = {
			.offset = 0,
			.length = region->size,
		},
	};
	int dmabuf_fd;

	dmabuf_fd = ioctl(fd, VFIO_DEVICE_FEATURE, &request);
	if (dmabuf_fd < 0)
		fprintf(stderr, "Cannot export BAR%u (%" PRIu64 " bytes) as DMABUF: %s\n",
			region->index, region->size, strerror(errno));
	return dmabuf_fd;
}

static int has_sparse_mmap(const struct vfio_region_info *info)
{
	uint32_t offset = info->cap_offset;
	size_t remaining = info->argsz / sizeof(struct vfio_info_cap_header);

	if (!(info->flags & VFIO_REGION_INFO_FLAG_CAPS))
		return 0;
	if (!offset)
		goto malformed;

	while (offset) {
		const struct vfio_info_cap_header *cap;

		/* Check only the chain headers; sparse area details are unused. */
		if (!remaining-- || offset < sizeof(*info) ||
		    offset % _Alignof(struct vfio_info_cap_header) ||
		    offset > info->argsz - sizeof(*cap))
			goto malformed;
		cap = (const void *)((const char *)info + offset);
		if (cap->id == VFIO_REGION_INFO_CAP_SPARSE_MMAP)
			return 1;
		offset = cap->next;
	}
	return 0;

malformed:
	fprintf(stderr, "Invalid capability chain for VFIO region %u\n", info->index);
	return -1;
}

int vfio_for_each_mappable_region(int fd, const struct vfio_device_info *info,
		int (*callback)(const struct vfio_region *region, void *data),
		void *data)
{
	for (uint32_t i = 0; i < info->num_regions; i++) {
		/* Include aligned stack storage for the mmap capability chain. */
		struct {
			struct vfio_region_info info;
			uint64_t capabilities[512];
		} raw = {
			.info = {
				.argsz = sizeof(raw),
				.index = i,
			},
		};
		struct vfio_region region;
		int ret;

		if (ioctl(fd, VFIO_DEVICE_GET_REGION_INFO, &raw) < 0) {
			fprintf(stderr, "VFIO_DEVICE_GET_REGION_INFO for region %u failed: %s\n",
				i, strerror(errno));
			continue;
		}
		if (raw.info.argsz < sizeof(raw.info) || raw.info.argsz > sizeof(raw)) {
			fprintf(stderr, "VFIO region %u info size %u does not fit the %zu-byte stack buffer\n",
				i, raw.info.argsz, sizeof(raw));
			return -1;
		}
		if (!raw.info.size || !(raw.info.flags & VFIO_REGION_INFO_FLAG_MMAP))
			continue;
		ret = has_sparse_mmap(&raw.info);
		if (ret < 0)
			return -1;
		if (ret)
			continue;
		region = (struct vfio_region) {
			.index = raw.info.index,
			.flags = raw.info.flags,
			.offset = raw.info.offset,
			.size = raw.info.size,
		};
		ret = callback(&region, data);
		if (ret)
			return ret;
	}
	return 0;
}
