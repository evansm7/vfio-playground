#ifndef VFIO_PLAYGROUND_VFIO_H
#define VFIO_PLAYGROUND_VFIO_H

#include <stdint.h>
#include <linux/vfio.h>

struct vfio_region {
	uint32_t index;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};

/* Return a bound device fd, or -1 with a diagnostic on stderr. The caller
 * owns both returned fds and must close the device before the iommufd.
 * Binding grants MMIO access; no DMA address space is attached.
 */
int vfio_open_cdev(const char *path, int *iommufd);

/* Return a VFIO device fd, or -1.  The caller figures out
 * the group number associated with the RID string passed in.
 */
int vfio_open_traditional(int groupnr, const char *rid_str);

int vfio_get_device_info(int fd, struct vfio_device_info *info);

/* Export the entire region as one DMABUF range. Return an owned fd, or -1
 * with a diagnostic. The caller must keep the device open while using it.
 */
int vfio_export_region_dmabuf(int fd, const struct vfio_region *region);

/* Visit nonempty regions advertising mmap support without sparse mmap caps. The region pointer is
 * valid only during the callback. Return 0 on success, -1 on error, or the
 * callback's nonzero return value to stop enumeration early.
 * Region info uses a stack buffer with 4 KiB of capability storage;
 * larger responses are reported as errors.
 */
int vfio_for_each_mappable_region(int fd, const struct vfio_device_info *info,
		int (*callback)(const struct vfio_region *region, void *data),
		void *data);

#endif
