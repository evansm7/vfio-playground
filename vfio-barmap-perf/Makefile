include ../common.mk
CFLAGS += -std=gnu17

BUILD_DIR := $(BUILD_ROOT)/vfio-barmap-perf
TARGET := $(BUILD_DIR)/vfio-barmap-perf
OBJS := $(BUILD_DIR)/main.o
DEPS := $(OBJS:.o=.d)
VFIO_LIB := $(BUILD_ROOT)/lib/libvfio.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(VFIO_LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Also support building directly from this directory.
.PHONY: $(VFIO_LIB)
$(VFIO_LIB):
	$(MAKE) -C ../lib

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) -r $(BUILD_DIR)

-include $(DEPS)
