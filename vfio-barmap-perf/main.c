#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "vfio.h"

#define MIN_BAR_SIZE (64ULL * 1024 * 1024)

struct bar_selection {
	struct vfio_region bar;
	int found;
};

static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "Usage: %s -d /dev/vfio/devices/vfioN\n"
		"  -d PATH  VFIO device cdev (required)\n"
		"  -h       Show this help\n", program);
}

static int select_bar(const struct vfio_region *region, void *data)
{
	struct bar_selection *selection = data;

	if (region->index > VFIO_PCI_BAR5_REGION_INDEX ||
	    !(region->flags & VFIO_REGION_INFO_FLAG_WRITE) ||
	    region->size < MIN_BAR_SIZE)
		return 0;

	selection->bar = *region;
	selection->found = 1;
	return 1;
}

static int time_pass(void *mapping, size_t length, size_t page_size,
		     uint64_t *elapsed_us)
{
	struct timeval start, end;
	uint64_t start_us, end_us;
	volatile unsigned char *base = mapping;

	if (gettimeofday(&start, NULL) < 0) {
		fprintf(stderr, "Cannot sample start time: %s\n", strerror(errno));
		return -1;
	}
	for (size_t offset = 0; offset < length; offset += page_size)
		*(volatile uint32_t *)(base + offset) = 0;
	if (gettimeofday(&end, NULL) < 0) {
		fprintf(stderr, "Cannot sample end time: %s\n", strerror(errno));
		return -1;
	}

	start_us = (uint64_t)start.tv_sec * 1000000 + start.tv_usec;
	end_us = (uint64_t)end.tv_sec * 1000000 + end.tv_usec;
	if (end_us < start_us) {
		fprintf(stderr, "System clock moved backwards during measurement\n");
		return -1;
	}
	*elapsed_us = end_us - start_us;
	return 0;
}

static int measure_bar(int fd, const struct vfio_region *bar)
{
	int page_size = getpagesize();
	size_t length, pages;
	void *mapping;
	uint64_t first_us, second_us, difference_us;
	int ret = -1;

	if (page_size <= 0 || page_size % sizeof(uint32_t)) {
		fprintf(stderr, "Invalid base page size: %d\n", page_size);
		return -1;
	}
	if (bar->size > SIZE_MAX || bar->offset > INT64_MAX ||
	    bar->size % page_size || bar->offset % page_size) {
		fprintf(stderr, "BAR%u size or offset cannot be mapped as whole system pages\n",
			bar->index);
		return -1;
	}
	length = bar->size;
	pages = length / page_size;

	/* Leave the VMA untouched until the first timed pass. */
	mapping = mmap(NULL, length, PROT_WRITE, MAP_SHARED, fd, (off_t)bar->offset);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "Cannot mmap BAR%u (%" PRIu64 " bytes): %s\n",
			bar->index, bar->size, strerror(errno));
		return -1;
	}

	if (time_pass(mapping, length, page_size, &first_us) < 0 ||
	    time_pass(mapping, length, page_size, &second_us) < 0)
		goto unmap;

	printf("BAR%u: %" PRIu64 " bytes, %zu pages, %d-byte base pages\n",
	       bar->index, bar->size, pages, page_size);
	printf("First access:  %" PRIu64 " us total, %" PRIu64 " ns/page\n",
	       first_us, first_us * 1000 / pages);
	printf("Second access: %" PRIu64 " us total, %" PRIu64 " ns/page\n",
	       second_us, second_us * 1000 / pages);
	if (second_us > 0) {
		uint64_t ratio_milli = first_us * 1000 / second_us;

		printf("First/second: %" PRIu64 ".%03" PRIu64 "x\n",
		       ratio_milli / 1000, ratio_milli % 1000);
	} else {
		printf("First/second: unavailable (second pass below timer resolution)\n");
	}
	difference_us = first_us >= second_us ? first_us - second_us : second_us - first_us;
	printf("Difference: %s%" PRIu64 " us (%s%" PRIu64 " ns/page)\n",
	       first_us < second_us ? "-" : "", difference_us,
	       first_us < second_us ? "-" : "", difference_us * 1000 / pages);
	ret = 0;

unmap:
	if (munmap(mapping, length) < 0) {
		fprintf(stderr, "Cannot unmap BAR%u: %s\n", bar->index, strerror(errno));
		ret = -1;
	}
	return ret;
}

int main(int argc, char **argv)
{
	struct vfio_device_info info;
	struct bar_selection selection = { 0 };
	const char *path = NULL;
	int fd, iommufd, option;
	int status = EXIT_FAILURE;

	opterr = 0;
	while ((option = getopt(argc, argv, "d:h")) != -1) {
		switch (option) {
		case 'd':
			path = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		default:
			fprintf(stderr, "Unknown option or missing option argument\n");
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (!path || !*path || optind != argc) {
		fprintf(stderr, "A VFIO cdev path must be supplied with -d, with no positional arguments\n");
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	fd = vfio_open_cdev(path, &iommufd);
	if (fd < 0)
		return EXIT_FAILURE;
	if (vfio_get_device_info(fd, &info) < 0)
		goto close_fds;
	if (!(info.flags & VFIO_DEVICE_FLAGS_PCI)) {
		fprintf(stderr, "%s is not a PCI VFIO device\n", path);
		goto close_fds;
	}
	if (vfio_for_each_mappable_region(fd, &info, select_bar, &selection) < 0)
		goto close_fds;
	if (!selection.found) {
		fprintf(stderr, "No writable, mappable PCI BAR without sparse mmap of at least %" PRIu64
			" MiB found on %s\n", (uint64_t)MIN_BAR_SIZE / 1024 / 1024, path);
		goto close_fds;
	}
	if (!measure_bar(fd, &selection.bar))
		status = EXIT_SUCCESS;

close_fds:
	close(fd);
	close(iommufd);
	return status;
}
