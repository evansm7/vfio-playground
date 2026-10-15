include common.mk

SUBDIRS := lib vfio-barmap-perf

.PHONY: all clean $(SUBDIRS)

all: $(SUBDIRS)

vfio-barmap-perf: lib

$(SUBDIRS):
	$(MAKE) -C $@

clean:
	$(RM) -r $(BUILD_ROOT)
