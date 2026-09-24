CXX := clang++
CC := clang
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -fobjc-arc
LDFLAGS :=

HEADLESS_FRAMEWORKS := -framework Foundation -framework QuartzCore -framework CoreAudio -framework AudioToolbox -framework AudioUnit
APP_FRAMEWORKS := $(HEADLESS_FRAMEWORKS) -framework Cocoa -framework UniformTypeIdentifiers
METAL_FRAMEWORKS := -framework Metal
GL_FRAMEWORKS := -framework OpenGL -framework CoreVideo

SRC_CPP := src/arena.cpp src/undo_stack.cpp src/scene.cpp src/audio.cpp src/timeline.cpp src/gizmo.cpp src/game.cpp src/scene_import.cpp src/blend_file.cpp src/menu.cpp src/ui.cpp src/app.cpp src/renderer_common.cpp src/font_atlas.cpp
HEADERS := $(wildcard src/*.h src/macos/*.h)

# Each binary links one graphics backend (renderer + UI renderer) and the
# matching presenter beside the shared AppKit platform file.
METAL_BACKEND := src/renderer_metal.mm src/ui_render_metal.mm
GL_BACKEND := src/renderer_gl.cpp src/ui_render_gl.cpp src/gl_shader.cpp
METAL_APP_SRC := $(SRC_CPP) $(METAL_BACKEND) src/macos/platform_macos.mm src/macos/platform_macos_metal.mm
GL_APP_SRC := $(SRC_CPP) $(GL_BACKEND) src/macos/platform_macos.mm src/macos/platform_macos_gl.mm

BUILD_DIR := build
ZSTD_OBJ := $(BUILD_DIR)/zstddeclib.o
TARGET := $(BUILD_DIR)/Renderer
GL_TARGET := $(BUILD_DIR)/Renderer_gl

UI_INSPECT_SRC := tools/ui_inspect.cpp src/ui.cpp src/scene.cpp src/timeline.cpp src/menu.cpp src/arena.cpp src/undo_stack.cpp

PARITY_SRC := tests/render_parity.cpp $(SRC_CPP)
PARITY_DIR := $(BUILD_DIR)/parity

.PHONY: all run opengl run-opengl clean ui-inspect parity-test bless-goldens

all: $(TARGET)

opengl: $(GL_TARGET)

ui-inspect: $(UI_INSPECT_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -Wall -Wextra -I src $(UI_INSPECT_SRC) -o $(BUILD_DIR)/ui_inspect
	$(BUILD_DIR)/ui_inspect

$(ZSTD_OBJ): src/third_party/zstd/zstddeclib.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -O2 -c $< -o $@

$(TARGET): $(METAL_APP_SRC) $(HEADERS) $(ZSTD_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(APP_FRAMEWORKS) $(METAL_FRAMEWORKS) $(METAL_APP_SRC) $(ZSTD_OBJ) $(LDFLAGS) -o $@

$(GL_TARGET): $(GL_APP_SRC) $(HEADERS) $(ZSTD_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(APP_FRAMEWORKS) $(GL_FRAMEWORKS) $(GL_APP_SRC) $(ZSTD_OBJ) $(LDFLAGS) -o $@

run: all
	$(TARGET)

run-opengl: opengl
	$(GL_TARGET)

$(BUILD_DIR)/parity_metal: $(PARITY_SRC) tests/offscreen_metal.mm $(METAL_BACKEND) $(HEADERS) tests/offscreen.h $(ZSTD_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I src $(HEADLESS_FRAMEWORKS) $(METAL_FRAMEWORKS) $(PARITY_SRC) tests/offscreen_metal.mm $(METAL_BACKEND) $(ZSTD_OBJ) $(LDFLAGS) -o $@

$(BUILD_DIR)/parity_gl: $(PARITY_SRC) tests/offscreen_gl.cpp $(GL_BACKEND) $(HEADERS) tests/offscreen.h $(ZSTD_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I src $(HEADLESS_FRAMEWORKS) -framework OpenGL $(PARITY_SRC) tests/offscreen_gl.cpp $(GL_BACKEND) $(ZSTD_OBJ) $(LDFLAGS) -o $@

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

# The Windows build cannot run Metal, so it diffs against these committed
# reference images instead. They are rendered with the AO pass disabled: SSAO
# does enough float work that two GPU vendors disagree on a few percent of the
# pixels, while everything else matches. Re-bless after any intended pixel
# change, and commit the result.
bless-goldens: $(BUILD_DIR)/parity_metal
	@rm -rf tests/golden
	@mkdir -p tests/golden
	$(BUILD_DIR)/parity_metal tests/golden --ao-off

clean:
	rm -rf $(BUILD_DIR)
