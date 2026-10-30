CC ?= gcc
CXX ?= g++
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -D_POSIX_C_SOURCE=200809L
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
DEPFLAGS = -MMD -MP
INCLUDES = -Iinclude

PKG_CFLAGS = $(shell pkg-config --cflags libcamera sdl2 SDL2_image SDL2_ttf)
PKG_LIBS   = $(shell pkg-config --libs libcamera sdl2 SDL2_image SDL2_ttf) -lpthread

BUILD_DIR ?= build

C_SRCS = src/main.c \
         src/text_renderer.c \
         src/dcf_storage.c \
         src/playback_manager.c

CXX_SRCS = src/camera_stream.cpp

C_OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS))
CXX_OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(CXX_SRCS))
OBJS = $(C_OBJS) $(CXX_OBJS)
DEPS = $(OBJS:.o=.d)

TARGET = $(BUILD_DIR)/rubuscam

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(PKG_LIBS)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) $(PKG_CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) src/*.o src/*.d rubuscam

-include $(DEPS)

.PHONY: all clean
