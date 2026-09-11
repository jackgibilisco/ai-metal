CXX := clang++
ZSTD_PREFIX := $(shell brew --prefix zstd)
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -fobjc-arc -I$(ZSTD_PREFIX)/include
LDFLAGS := -L$(ZSTD_PREFIX)/lib -lzstd

HEADLESS_FRAMEWORKS := -framework Foundation -framework QuartzCore -framework CoreAudio -framework AudioToolbox -framework AudioUnit -framework CoreText -framework CoreGraphics
APP_FRAMEWORKS := $(HEADLESS_FRAMEWORKS) -framework Cocoa -framework UniformTypeIdentifiers
METAL_FRAMEWORKS := -framework Metal
GL_FRAMEWORKS := -framework OpenGL -framework CoreVideo

SRC_CPP := src/arena.cpp src/undo_stack.cpp src/scene.cpp src/audio.cpp src/timeline.cpp src/gizmo.cpp src/game.cpp src/scene_import.cpp src/blend_file.cpp src/menu.cpp src/ui.cpp src/app.cpp src/renderer_common.cpp src/font_atlas.cpp
HEADERS := $(wildcard src/*.h)

# Each binary links one graphics backend (renderer + UI renderer) and the
# matching presenter beside the shared AppKit platform file.
METAL_BACKEND := src/renderer_metal.mm src/ui_render_metal.mm
GL_BACKEND := src/renderer_gl.cpp src/ui_render_gl.cpp src/gl_shader.cpp
METAL_APP_SRC := $(SRC_CPP) $(METAL_BACKEND) src/platform_macos.mm src/platform_macos_metal.mm
GL_APP_SRC := $(SRC_CPP) $(GL_BACKEND) src/platform_macos.mm src/platform_macos_gl.mm

BUILD_DIR := build
TARGET := $(BUILD_DIR)/Renderer
GL_TARGET := $(BUILD_DIR)/Renderer_gl

UI_INSPECT_SRC := tools/ui_inspect.cpp src/ui.cpp src/scene.cpp src/timeline.cpp src/menu.cpp src/arena.cpp src/undo_stack.cpp

PARITY_SRC := tests/render_parity.cpp $(SRC_CPP)
PARITY_DIR := $(BUILD_DIR)/parity

.PHONY: all run opengl run-opengl clean ui-inspect parity-test

all: $(TARGET)

opengl: $(GL_TARGET)

ui-inspect: $(UI_INSPECT_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -Wall -Wextra -I src $(UI_INSPECT_SRC) -framework CoreText -framework CoreGraphics -framework CoreFoundation -o $(BUILD_DIR)/ui_inspect
	$(BUILD_DIR)/ui_inspect

$(TARGET): $(METAL_APP_SRC) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(APP_FRAMEWORKS) $(METAL_FRAMEWORKS) $(METAL_APP_SRC) $(LDFLAGS) -o $@

$(GL_TARGET): $(GL_APP_SRC) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(APP_FRAMEWORKS) $(GL_FRAMEWORKS) $(GL_APP_SRC) $(LDFLAGS) -o $@

run: all
	$(TARGET)

run-opengl: opengl
	$(GL_TARGET)

$(BUILD_DIR)/parity_metal: $(PARITY_SRC) tests/offscreen_metal.mm $(METAL_BACKEND) $(HEADERS) tests/offscreen.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I src $(HEADLESS_FRAMEWORKS) $(METAL_FRAMEWORKS) $(PARITY_SRC) tests/offscreen_metal.mm $(METAL_BACKEND) $(LDFLAGS) -o $@

$(BUILD_DIR)/parity_gl: $(PARITY_SRC) tests/offscreen_gl.cpp $(GL_BACKEND) $(HEADERS) tests/offscreen.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I src $(HEADLESS_FRAMEWORKS) -framework OpenGL $(PARITY_SRC) tests/offscreen_gl.cpp $(GL_BACKEND) $(LDFLAGS) -o $@

$(BUILD_DIR)/image_diff: tests/image_diff.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -Wall -Wextra -O2 $< -o $@

# Renders the same scripted cases through both backends and diffs the images.
# Apple's GL has no GPU timer (its timer queries read 0), so GL timings are skipped.
parity-test: $(BUILD_DIR)/parity_metal $(BUILD_DIR)/parity_gl $(BUILD_DIR)/image_diff
	@rm -rf $(PARITY_DIR)/metal $(PARITY_DIR)/gl
	@mkdir -p $(PARITY_DIR)/metal $(PARITY_DIR)/gl
	$(BUILD_DIR)/parity_metal $(PARITY_DIR)/metal
	$(BUILD_DIR)/parity_gl $(PARITY_DIR)/gl
	$(BUILD_DIR)/image_diff $(PARITY_DIR)/metal $(PARITY_DIR)/gl --no-gpu-timings

clean:
	rm -rf $(BUILD_DIR)
