# vfio-barmap-perf

Build from the project root with `make`, then run:

```sh
./vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0
```

The tool opens the cdev and binds it to `/dev/iommu`. It selects the first
writable, mappable PCI BAR of at least 64 MiB without a sparse mmap
capability, then maps the entire BAR with `MAP_SHARED` without prefaulting.
Any BAR advertising sparse mmap is skipped, regardless of its area layout.

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
