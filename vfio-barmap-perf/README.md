# vfio-barmap-perf

Build from the project root with `make`, then run:

```sh
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0
```

All binaries, objects, dependency files, and the common VFIO library are
created under the top-level `build/` directory. `make clean` removes it.

The build checks the compiler's default kernel headers for the required
VFIO cdev and DMABUF definitions. If those headers are too old, specify
an absolute path to a UAPI include directory containing `linux/vfio.h`:

```sh
make KERNEL_HEADERS="$(realpath ../linux/include/uapi)"
```

The tool opens the cdev and binds it to `/dev/iommu`. It selects the first
writable, mappable PCI BAR of at least 64 MiB without a sparse mmap
capability, then maps the entire BAR with `MAP_SHARED` without prefaulting.
Any BAR advertising sparse mmap is skipped, regardless of its area layout.

Pass `--map-dmabuf` to export a DMABUF covering the entire selected BAR
and mmap that fd at offset zero instead of mapping the VFIO cdev:

```sh
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 --map-dmabuf
```

The kernel and device driver must support VFIO DMABUF export and mmap.
The export uses one range at BAR offset zero with the full BAR length and
the default uncached MMIO attribute. Export and mapping occur before timing;
both mapping modes use the same timed loops.

Pass `--multithreaded` to measure concurrent initial faults:

```sh
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 --multithreaded
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 --map-dmabuf --multithreaded
```

This mode forks before mapping the BAR. The parent and child inherit the
same VFIO or exported DMABUF fd, then each maps its own untouched VMA and
runs one cold pass. A shared anonymous mapping holds lock-free C17 atomics:
the child announces that its VMA is ready and spins until the parent
releases both walkers. The barrier uses atomic loads/stores and CPU spin
hints, without syscalls or pipes. Mapping failures abort the handshake.
The parent waits for the child after its pass and reports both times.
Scheduling can still introduce start skew; simultaneous execution requires
both processes to be scheduled on different CPUs.

In the default mode, each of two consecutive passes writes a volatile
32-bit zero at the start
of every base system page. Both loops are timed with `gettimeofday()`;
output includes elapsed microseconds, nanoseconds per page, and the ratio
and difference between the passes.

The first pass includes initial mapping faults; the second reuses the
mapping. The timing difference also includes effects such as TLB state
and scheduling. Base page steps do not imply one fault per step: the
kernel may install larger mappings. Timing measures the CPU issuing the
stores, without a device readback to drain posted writes.

Assisted-by: OpenAI Codex (GPT-6 LLM)
