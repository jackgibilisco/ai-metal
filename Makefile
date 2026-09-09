CXX := clang++
ZSTD_PREFIX := $(shell brew --prefix zstd)
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -fobjc-arc -I$(ZSTD_PREFIX)/include
FRAMEWORKS := -framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore -framework UniformTypeIdentifiers -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreText -framework CoreGraphics
LDFLAGS := -L$(ZSTD_PREFIX)/lib -lzstd

SRC_CPP := src/arena.cpp src/undo_stack.cpp src/scene.cpp src/audio.cpp src/timeline.cpp src/gizmo.cpp src/game.cpp src/scene_import.cpp src/blend_file.cpp src/menu.cpp src/ui.cpp src/app.cpp
SRC_MM := src/renderer_metal.mm src/ui_render_metal.mm src/platform_macos.mm

BUILD_DIR := build
TARGET := $(BUILD_DIR)/Renderer

UI_INSPECT_SRC := tools/ui_inspect.cpp src/ui.cpp src/scene.cpp src/timeline.cpp src/menu.cpp src/arena.cpp src/undo_stack.cpp

.PHONY: all run clean ui-inspect

all: $(TARGET)

ui-inspect: $(UI_INSPECT_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -Wall -Wextra -I src $(UI_INSPECT_SRC) -framework CoreText -framework CoreGraphics -framework CoreFoundation -o $(BUILD_DIR)/ui_inspect
	$(BUILD_DIR)/ui_inspect

$(TARGET): $(SRC_CPP) $(SRC_MM)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(FRAMEWORKS) $(SRC_CPP) $(SRC_MM) $(LDFLAGS) -o $(TARGET)

run: all
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
