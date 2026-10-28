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
of every base system page. Both loops are timed with
`clock_gettime(CLOCK_MONOTONIC)`; output includes elapsed microseconds,
nanoseconds per page, and the ratio and difference between the passes.

The first pass includes initial mapping faults; the second reuses the
mapping. The timing difference also includes effects such as TLB state
and scheduling. Base page steps do not imply one fault per step: the
kernel may install larger mappings. Timing measures the CPU issuing the
stores, without a device readback to drain posted writes.

## Windows, page sizes and stride

`-o OFFSET` and `-l LENGTH` (K/M/G suffixes accepted) restrict the test
to a window of the selected BAR, so one large BAR can stand in for
smaller ones, or a large BAR can be measured without touching all of it.

The THP sysfs `enabled` setting does not stop huge faults on VFIO PFNMAP
VMAs. `--thp-disable` sets `PR_SET_THP_DISABLE` before mapping, which
does, so every fault installs a 4K PTE.

`mmap(NULL, ...)` returns a VA with no particular alignment to the BAR,
so whether faults could install PMD or PUD mappings would be left to
chance. The tool instead reserves extra VA and maps the window with
`MAP_FIXED` at a VA chosen relative to the BAR address. `--align pte`
(the default) offsets it by 4K modulo 2M, so every fault installs a 4K
PTE while the kernel still attempts, and falls back from, huge faults.
`--align pmd` and `--align pud` make it congruent modulo 2M or 1G (pmd
stays off 1G boundaries), so faults install 2M or 1G mappings.
`--align any` leaves placement to `mmap()`. The BAR is assumed naturally
aligned, as PCI requires.

`--stride SIZE` writes once per SIZE bytes instead of once per base page.
With huge mappings use `--stride 2M` or `--stride 1G`, so that each step
takes exactly one fault and the store loop does not dominate the passes.
"ns/page" is then per stride step.

```sh
# 128GB of 4K, 2M and 1G mappings, starting 4GB into the BAR
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 -o 4G -l 128G --thp-disable
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 -o 4G -l 128G --align pmd --stride 2M
./build/vfio-barmap-perf/vfio-barmap-perf -d /dev/vfio/devices/vfio0 -o 4G -l 128G --align pud --stride 1G
```

## Misc notes:

Unbind an endpoint from an existing driver, then bind to VFIO, then
find the CDEV path:

```
   # # might need this:
   # echo 1 > /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts
   #
   # lspci -n -s 0000:06:0d.0
   06:0d.0 0401: 1102:0002 (rev 08)
   #
   # echo 0000:06:0d.0 > /sys/bus/pci/devices/0000:06:0d.0/driver/unbind
   # echo 1102 0002 > /sys/bus/pci/drivers/vfio-pci/new_id
   # ls /sys/bus/pci/devices/0000\:06\:0d.0/vfio-dev
   vfio0
   #
   # ./vfio-barmap-perf /dev/vfio/devices/vfio0
```

Clanker direction, review, by Matt Evans.
Assisted-by: OpenAI Codex (GPT-6 LLM)
