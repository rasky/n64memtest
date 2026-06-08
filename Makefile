N64_ROM_ELFCOMPRESS=3
all: n64memtest.z64
.PHONY: all

BUILD_DIR = build
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/mem_tests.o $(BUILD_DIR)/logo.o $(BUILD_DIR)/mem_test_engine.o

n64memtest.z64: N64_ROM_TITLE = "N64 Memory Test"

$(BUILD_DIR)/n64memtest.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)

