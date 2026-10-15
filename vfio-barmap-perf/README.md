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

Each of two consecutive passes writes a volatile 32-bit zero at the start
of every base system page. Both loops are timed with `gettimeofday()`;
output includes elapsed microseconds, nanoseconds per page, and the ratio
and difference between the passes.

The first pass includes initial mapping faults; the second reuses the
mapping. The timing difference also includes effects such as TLB state
and scheduling. Base page steps do not imply one fault per step: the
kernel may install larger mappings. Timing measures the CPU issuing the
stores, without a device readback to drain posted writes.

Assisted-by: OpenAI Codex (GPT-6 LLM)
