SUBDIRS := lib vfio-barmap-perf

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

vfio-barmap-perf: lib

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	@for dir in $(SUBDIRS); do $(MAKE) -C $$dir clean || exit $$?; done
