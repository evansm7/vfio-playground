#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vfio.h"

#define MIN_BAR_SIZE (64ULL * 1024 * 1024)

struct bar_selection {
	struct vfio_region bar;
	int found;
};

struct cold_result {
	uint64_t elapsed_ns;
	atomic_int status;
};

struct cold_state {
	/* Keep the handshake stores on separate cache lines. */
	_Alignas(64) atomic_uint ready;
	_Alignas(64) atomic_uint start;
	_Alignas(64) atomic_uint abort;
	_Alignas(64) struct cold_result result[2];
};

static void usage(FILE *stream, const char *program)
{
	fprintf(stream, "Usage: %s -d /dev/vfio/devices/vfioN [--map-dmabuf] [--multithreaded]\n"
		"  -d PATH          VFIO device cdev (require either this, or...)\n"
		"  -r RID           Target endpoint RID, e.g. 0000:01:02.3 (...this, needs group)\n"
		"  -g <group>       IOMMU group (required when -r used)\n"
		"  --map-dmabuf     Export and mmap a DMABUF covering the entire BAR\n"
		"  --multithreaded  Run concurrent cold passes in two processes\n"
		"  -o OFFSET        Map only from this BAR offset (K/M/G suffix ok)\n"
		"  -l LENGTH        Map only this many bytes of the BAR (default: to end)\n"
		"  --thp-disable    PR_SET_THP_DISABLE before mapping (4K faults only)\n"
		"  --align MODE     VA placement vs. the BAR: pte (default; never huge),\n"
		"                   pmd (2M mappings), pud (1G mappings), any (mmap() picks)\n"
		"  --stride SIZE    Write once per SIZE bytes (default: base page size)\n"
		"  -h               Show this help\n", program);
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

static uint64_t win_offset, win_length, stride;
/* VA minus BAR address, modulo 1G; ALIGN_ANY leaves placement to mmap(). */
#define ALIGN_PTE	(4ULL << 10)
#define ALIGN_PMD	(2ULL << 20)
#define ALIGN_PUD	0ULL
#define ALIGN_ANY	(~0ULL)
static uint64_t align_skew = ALIGN_PTE;

/*
 * Map [offset, offset+length) of fd at a VA chosen relative to the BAR.
 * mmap(NULL, ...) gives no alignment, so whether faults could use PMD or
 * PUD mappings would be left to chance.  Reserve 1G of extra VA and
 * MAP_FIXED the window inside it at the wanted skew: 4K (pte) can never be
 * mapped huge, 2M (pmd) allows PMDs but no PUDs, 0 (pud) allows PUDs.  PCI
 * BARs are naturally aligned, so for any BAR big enough to hold a huge page
 * its address is congruent with win_offset.
 */
static void *map_window(int fd, off_t offset, size_t length)
{
	const uint64_t G = 1ULL << 30;
	size_t res_len = length + G;
	uintptr_t res, va;
	void *p;

	if (align_skew == ALIGN_ANY)
		return mmap(NULL, length, PROT_WRITE, MAP_SHARED, fd, offset);
	p = mmap(NULL, res_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return MAP_FAILED;
	res = (uintptr_t)p;
	va = res + (((win_offset + align_skew) % G - res % G + G) % G);
	p = mmap((void *)va, length, PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset);
	if (p == MAP_FAILED) {
		int err = errno;

		munmap((void *)res, res_len);
		errno = err;
		return MAP_FAILED;
	}
	if (va > res)
		munmap((void *)res, va - res);
	if (va + length < res + res_len)
		munmap((void *)(va + length), res + res_len - va - length);
	return p;
}

static int time_pass(void *mapping, size_t length, size_t step,
		     uint64_t *elapsed_ns)
{
	struct timespec start, end;
	uint64_t start_ns, end_ns;
	volatile unsigned char *base = mapping;

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
		fprintf(stderr, "Cannot sample start time: %s\n", strerror(errno));
		return -1;
	}
	for (size_t offset = 0; offset < length; offset += step)
		*(volatile uint32_t *)(base + offset) = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
		fprintf(stderr, "Cannot sample end time: %s\n", strerror(errno));
		return -1;
	}

	start_ns = (uint64_t)start.tv_sec * 1000000000 + start.tv_nsec;
	end_ns = (uint64_t)end.tv_sec * 1000000000 + end.tv_nsec;
	if (end_ns < start_ns) {
		fprintf(stderr, "System clock moved backwards during measurement\n");
		return -1;
	}
	*elapsed_ns = end_ns - start_ns;
	return 0;
}

static void spin_hint(void)
{
#if defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	atomic_signal_fence(memory_order_seq_cst);
#endif
}

static int cold_barrier(struct cold_state *state, int slot)
{
	if (slot == 0) {
		/* The parent releases both walkers once the child has its VMA. */
		while (!atomic_load_explicit(&state->ready, memory_order_acquire)) {
			if (atomic_load_explicit(&state->abort, memory_order_acquire))
				return -1;
			spin_hint();
		}
		if (atomic_load_explicit(&state->abort, memory_order_acquire))
			return -1;
		atomic_store_explicit(&state->start, 1, memory_order_release);
	} else {
		atomic_store_explicit(&state->ready, 1, memory_order_release);
		while (!atomic_load_explicit(&state->start, memory_order_acquire)) {
			if (atomic_load_explicit(&state->abort, memory_order_acquire))
				return -1;
			spin_hint();
		}
	}
	return 0;
}

static int cold_process(struct cold_state *state, int slot, int fd,
			off_t offset, size_t length, size_t step)
{
	void *mapping;
	int ret = -1;

	/* This mmap happens after fork, giving each process cold page tables. */
	mapping = map_window(fd, offset, length);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "%s cannot mmap BAR for cold pass: %s\n",
			slot ? "Child" : "Parent", strerror(errno));
		goto done;
	}
	if (!cold_barrier(state, slot))
		ret = time_pass(mapping, length, step, &state->result[slot].elapsed_ns);
	if (munmap(mapping, length) < 0) {
		fprintf(stderr, "%s cannot unmap BAR: %s\n",
			slot ? "Child" : "Parent", strerror(errno));
		ret = -1;
	}

done:
	if (ret)
		atomic_store_explicit(&state->abort, 1, memory_order_release);
	atomic_store_explicit(&state->result[slot].status, ret, memory_order_release);
	return ret;
}

static int measure_concurrent(int fd, off_t offset, const struct vfio_region *bar,
			      size_t step, bool map_dmabuf)
{
	struct cold_state *state;
	pid_t child, waited;
	size_t pages = bar->size / step;
	int child_status, ret = -1;

	/* Only the handshake/result memory is mapped before fork. */
	state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (state == MAP_FAILED) {
		fprintf(stderr, "Cannot map shared cold-pass state: %s\n", strerror(errno));
		return -1;
	}
	atomic_init(&state->ready, 0);
	atomic_init(&state->start, 0);
	atomic_init(&state->abort, 0);
	for (int i = 0; i < 2; i++)
		atomic_init(&state->result[i].status, -1);
	if (!atomic_is_lock_free(&state->ready) ||
	    !atomic_is_lock_free(&state->result[0].status)) {
		fprintf(stderr, "Concurrent mode requires lock-free shared-memory atomics\n");
		goto unmap_state;
	}

	child = fork();
	if (child < 0) {
		fprintf(stderr, "Cannot fork cold-pass process: %s\n", strerror(errno));
		goto unmap_state;
	}
	if (child == 0) {
		int status = cold_process(state, 1, fd, offset, bar->size, step);

		/* Process exit releases the inherited fds and shared state mapping. */
		_exit(status ? EXIT_FAILURE : EXIT_SUCCESS);
	}

	cold_process(state, 0, fd, offset, bar->size, step);
	do {
		waited = waitpid(child, &child_status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited < 0) {
		fprintf(stderr, "Cannot wait for cold-pass child: %s\n", strerror(errno));
		goto unmap_state;
	}
	if (WIFSIGNALED(child_status)) {
		fprintf(stderr, "Cold-pass child terminated by signal %d\n",
			WTERMSIG(child_status));
		goto unmap_state;
	}
	if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) ||
	    atomic_load_explicit(&state->result[0].status, memory_order_acquire) ||
	    atomic_load_explicit(&state->result[1].status, memory_order_acquire))
		goto unmap_state;

	printf("BAR%u: %" PRIu64 " bytes, %zu pages, %zu-byte stride\n",
	       bar->index, bar->size, pages, step);
	printf("Mapping: %s; concurrent cold passes in two processes\n",
	       map_dmabuf ? "DMABUF" : "VFIO cdev");
	printf("Parent cold access (PID %ld): %.3f us total, %.1f ns/page\n",
	       (long)getpid(), state->result[0].elapsed_ns / 1e3,
	       (double)state->result[0].elapsed_ns / pages);
	printf("Child cold access (PID %ld):  %.3f us total, %.1f ns/page\n",
	       (long)child, state->result[1].elapsed_ns / 1e3,
	       (double)state->result[1].elapsed_ns / pages);
	ret = 0;

unmap_state:
	if (munmap(state, sizeof(*state)) < 0) {
		fprintf(stderr, "Cannot unmap shared cold-pass state: %s\n", strerror(errno));
		ret = -1;
	}
	return ret;
}

static uint64_t parse_size(const char *arg)
{
	char *end;
	uint64_t v = strtoull(arg, &end, 0);

	switch (*end) {
	case 'G': case 'g': v <<= 10; /* fallthrough */
	case 'M': case 'm': v <<= 10; /* fallthrough */
	case 'K': case 'k': v <<= 10;
	}
	return v;
}

static int measure_bar(int fd, const struct vfio_region *full_bar, bool map_dmabuf,
		       bool multithreaded)
{
	struct vfio_region view = *full_bar;
	const struct vfio_region *bar = &view;
	int page_size = getpagesize();
	size_t length, pages;
	void *mapping;
	uint64_t first_ns, second_ns, difference_ns;
	size_t step;
	int map_fd = fd, dmabuf_fd = -1;
	off_t map_offset = 0;
	int ret = -1;

	if (page_size <= 0 || page_size % sizeof(uint32_t)) {
		fprintf(stderr, "Invalid base page size: %d\n", page_size);
		return -1;
	}
	if (bar->size > SIZE_MAX || bar->size % page_size ||
	    (!map_dmabuf && (bar->offset > INT64_MAX || bar->offset % page_size))) {
		fprintf(stderr, "BAR%u size or offset cannot be mapped as whole system pages\n",
			bar->index);
		return -1;
	}
	if (win_offset % page_size || win_offset >= full_bar->size ||
	    (win_length && (win_length % page_size || win_length > full_bar->size - win_offset))) {
		fprintf(stderr, "Window offset/length outside BAR%u or not page aligned\n",
			bar->index);
		return -1;
	}
	view.size = win_length ? win_length : full_bar->size - win_offset;
	length = bar->size;
	if (stride % page_size || stride > length) {
		fprintf(stderr, "Stride must be a multiple of the page size and fit the window\n");
		return -1;
	}
	step = stride ? stride : (size_t)page_size;
	pages = length / step;
	printf("Window: BAR offset 0x%" PRIx64 ", length 0x%zx\n", win_offset, length);

	if (map_dmabuf) {
		dmabuf_fd = vfio_export_region_dmabuf(fd, full_bar);
		if (dmabuf_fd < 0)
			return -1;
		map_fd = dmabuf_fd;
		map_offset = win_offset;
	} else {
		map_offset = bar->offset + win_offset;
	}

	if (multithreaded) {
		ret = measure_concurrent(map_fd, map_offset, bar, step, map_dmabuf);
		goto close_dmabuf;
	}

	/* Leave the VMA untouched until the first timed pass. */
	mapping = map_window(map_fd, map_offset, length);
	if (mapping == MAP_FAILED) {
		fprintf(stderr, "Cannot mmap BAR%u via %s (%" PRIu64 " bytes): %s\n",
			bar->index, map_dmabuf ? "DMABUF" : "VFIO cdev",
			bar->size, strerror(errno));
		goto close_dmabuf;
	}

	if (time_pass(mapping, length, step, &first_ns) < 0 ||
	    time_pass(mapping, length, step, &second_ns) < 0)
		goto unmap;

	printf("BAR%u: %" PRIu64 " bytes, %zu pages, %zu-byte stride\n",
	       bar->index, bar->size, pages, step);
	printf("Mapping: %s\n", map_dmabuf ? "DMABUF" : "VFIO cdev");
	printf("First access:  %.3f us total, %.1f ns/page\n",
	       first_ns / 1e3, (double)first_ns / pages);
	printf("Second access: %.3f us total, %.1f ns/page\n",
	       second_ns / 1e3, (double)second_ns / pages);
	if (second_ns > 0) {
		uint64_t ratio_milli = first_ns * 1000 / second_ns;

		printf("First/second: %" PRIu64 ".%03" PRIu64 "x\n",
		       ratio_milli / 1000, ratio_milli % 1000);
	} else {
		printf("First/second: unavailable (second pass below timer resolution)\n");
	}
	difference_ns = first_ns >= second_ns ? first_ns - second_ns : second_ns - first_ns;
	printf("Difference: %s%.3f us (%s%.1f ns/page)\n",
	       first_ns < second_ns ? "-" : "", difference_ns / 1e3,
	       first_ns < second_ns ? "-" : "", (double)difference_ns / pages);
	ret = 0;

unmap:
	if (munmap(mapping, length) < 0) {
		fprintf(stderr, "Cannot unmap BAR%u: %s\n", bar->index, strerror(errno));
		ret = -1;
	}
close_dmabuf:
	if (dmabuf_fd >= 0)
		close(dmabuf_fd);
	return ret;
}

int main(int argc, char **argv)
{
	enum { OPT_MAP_DMABUF = 256, OPT_MULTITHREADED, OPT_THP_DISABLE, OPT_ALIGN, OPT_STRIDE };
	static const struct option long_options[] = {
		{ "map-dmabuf", no_argument, NULL, OPT_MAP_DMABUF },
		{ "multithreaded", no_argument, NULL, OPT_MULTITHREADED },
		{ "thp-disable", no_argument, NULL, OPT_THP_DISABLE },
		{ "align", required_argument, NULL, OPT_ALIGN },
		{ "stride", required_argument, NULL, OPT_STRIDE },
		{ 0 },
	};
	struct vfio_device_info info;
	struct bar_selection selection = { 0 };
	const char *path = NULL;
	const char *rid = NULL;
	int group = -1;
	bool map_dmabuf = false;
	bool multithreaded = false;
	bool thp_disable = false;
	int fd, option;
	int iommufd = -1;
	int status = EXIT_FAILURE;

	opterr = 0;
	while ((option = getopt_long(argc, argv, "d:r:g:o:l:h", long_options, NULL)) != -1) {
		switch (option) {
		case 'd':
			path = optarg;
			break;
		case 'r':
			rid = optarg;
			break;
		case 'g':
			group = atoi(optarg);
			break;
		case 'h':
			usage(stdout, argv[0]);
			return EXIT_SUCCESS;
		case OPT_MAP_DMABUF:
			map_dmabuf = true;
			break;
		case OPT_MULTITHREADED:
			multithreaded = true;
			break;
		case OPT_THP_DISABLE:
			thp_disable = true;
			break;
		case OPT_ALIGN:
			if (!strcmp(optarg, "pte"))
				align_skew = ALIGN_PTE;
			else if (!strcmp(optarg, "pmd"))
				align_skew = ALIGN_PMD;
			else if (!strcmp(optarg, "pud"))
				align_skew = ALIGN_PUD;
			else if (!strcmp(optarg, "any"))
				align_skew = ALIGN_ANY;
			else {
				fprintf(stderr, "--align takes pte, pmd, pud or any\n");
				return EXIT_FAILURE;
			}
			break;
		case OPT_STRIDE:
			stride = parse_size(optarg);
			break;
		case 'o':
			win_offset = parse_size(optarg);
			break;
		case 'l':
			win_length = parse_size(optarg);
			break;
		default:
			fprintf(stderr, "Unknown option or missing option argument\n");
			usage(stderr, argv[0]);
			return EXIT_FAILURE;
		}
	}
	if ((path && (rid || group != -1)) || (!!rid != (group != -1))) {
		fprintf(stderr, "Supply either a cdev path, or both a RID + group\n");
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
	if (!rid && (!path || !*path)) {
		fprintf(stderr, "A VFIO cdev path must be supplied with -d, with no positional arguments\n");
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}
	if (thp_disable && (align_skew == ALIGN_PMD || align_skew == ALIGN_PUD)) {
		fprintf(stderr, "--thp-disable rules out --align pmd/pud\n");
		return EXIT_FAILURE;
	}
	/*
	 * The THP sysfs "enabled" knob does not stop huge_fault on PFNMAP VMAs;
	 * the prctl does, and it is inherited across fork().
	 */
	if (thp_disable && prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0)) {
		perror("PR_SET_THP_DISABLE");
		return EXIT_FAILURE;
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	const char *devname;
	if (path) {
		devname = path;
		fd = vfio_open_cdev(path, &iommufd);
	} else {
		devname = rid;
		fd = vfio_open_traditional(group, rid);
	}

	if (fd < 0)
		return EXIT_FAILURE;
	if (vfio_get_device_info(fd, &info) < 0)
		goto close_fds;
	if (!(info.flags & VFIO_DEVICE_FLAGS_PCI)) {
		fprintf(stderr, "%s is not a PCI VFIO device\n", devname);
		goto close_fds;
	}
	if (vfio_for_each_mappable_region(fd, &info, select_bar, &selection) < 0)
		goto close_fds;
	if (!selection.found) {
		fprintf(stderr, "No writable, mappable PCI BAR without sparse mmap of at least %" PRIu64
			" MiB found on %s\n", (uint64_t)MIN_BAR_SIZE / 1024 / 1024, devname);
		goto close_fds;
	}
	if (!measure_bar(fd, &selection.bar, map_dmabuf, multithreaded))
		status = EXIT_SUCCESS;

close_fds:
	close(fd);
	if (fd != -1)
		close(iommufd);
	return status;
}
