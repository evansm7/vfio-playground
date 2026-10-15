include ../common.mk

TARGET := vfio-barmap-perf
OBJS := main.o
DEPS := $(OBJS:.o=.d)
VFIO_LIB := ../lib/libvfio.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(VFIO_LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Also support building directly from this directory.
.PHONY: $(VFIO_LIB)
$(VFIO_LIB):
	$(MAKE) -C ../lib

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) $(TARGET) $(OBJS) $(DEPS)

-include $(DEPS)
