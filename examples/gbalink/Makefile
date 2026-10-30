all: gbalink.z64
.PHONY: all

BUILD_DIR = build
# This example uses GBA Link Cable, which is a preview API
LIBDRAGON_PREVIEW = 2
include $(N64_INST)/include/n64.mk

OBJS = $(BUILD_DIR)/gbalink.o
assets_gba = $(wildcard assets/*.gba)

assets_conv = $(addprefix filesystem/,$(notdir $(assets_gba)))

filesystem/%.gba: assets/%.gba
	@mkdir -p $(dir $@)
	@echo "    [MKASSET] $@"
	$(N64_BINDIR)/mkasset -c 2 -w 256 -o filesystem/ $<

$(BUILD_DIR)/gbalink.dfs: $(assets_conv)

gbalink.z64: N64_ROM_TITLE = "GBA Link Cable"
gbalink.z64: $(BUILD_DIR)/gbalink.dfs

$(BUILD_DIR)/gbalink.elf: $(OBJS)

clean:
	rm -rf $(BUILD_DIR) filesystem/ *.z64
.PHONY: clean

-include $(wildcard $(BUILD_DIR)/*.d)
