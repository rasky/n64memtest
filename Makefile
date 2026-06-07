N64_ROM_ELFCOMPRESS=3
all: n64memtest.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/n64memtest.o $(BUILD_DIR)/mem_tests.o $(BUILD_DIR)/moving_stack.o $(BUILD_DIR)/mem_slice_critical.o

n64memtest.z64: N64_ROM_TITLE = "N64 Memory Test"

$(BUILD_DIR)/n64memtest.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
