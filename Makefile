# LanDrop - Linux build
#
# Requirements: g++ (C++17), Xlib, Xft and fontconfig development headers.
#   Ubuntu/Debian: sudo apt install build-essential libx11-dev libxft-dev libfontconfig1-dev
#   Fedora:        sudo dnf install gcc-c++ libX11-devel libXft-devel fontconfig-devel
#
# For a 32-bit build on a multilib system:
#   make ARCH_FLAGS=-m32

CXX ?= g++
PREFIX ?= /usr/local
ARCH_FLAGS ?=
TARGET := build/landrop
TEST_TARGET := build/landrop-tests
DESKTOP_FILE := resources/landrop.desktop
ICON_FILE := resources/landrop.svg

PKG_CONFIG ?= pkg-config
X11_CFLAGS := $(shell $(PKG_CONFIG) --cflags x11 xft fontconfig 2>/dev/null)
X11_LIBS := $(shell $(PKG_CONFIG) --libs x11 xft fontconfig 2>/dev/null)
ifeq ($(strip $(X11_LIBS)),)
X11_LIBS := -lX11 -lXft -lfontconfig
endif

CPPFLAGS := -Isrc -D_FILE_OFFSET_BITS=64 -DNDEBUG $(X11_CFLAGS)
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -Wpedantic -pthread $(ARCH_FLAGS)
GXX_MAJOR := $(shell $(CXX) -dumpversion | cut -d. -f1)
FS_LIBS := $(shell test "$(GXX_MAJOR)" -ge 9 2>/dev/null || echo -lstdc++fs)
LDFLAGS := $(ARCH_FLAGS) -pthread -s
LDLIBS := $(X11_LIBS) $(FS_LIBS)

CORE_SOURCES := \
	src/core/sha256.cpp \
	src/core/util.cpp \
	src/core/protocol.cpp \
	src/core/socket.cpp \
	src/core/scanner.cpp \
	src/core/sender.cpp \
	src/core/receiver.cpp \
	src/core/discovery.cpp \
	src/core/settings.cpp \
	src/core/state.cpp \
	src/core/app_controller.cpp \
	src/platform/file_io.cpp \
	src/platform/platform_posix.cpp

APP_SOURCES := $(CORE_SOURCES) src/ui/ui_x11.cpp src/main.cpp
TEST_SOURCES := $(CORE_SOURCES) tests/test_core.cpp
HEADERS := $(wildcard src/*.h src/core/*.h src/platform/*.h src/ui/*.h)

.PHONY: all test clean run install

all: $(TARGET)

$(TARGET): $(APP_SOURCES) $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(APP_SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): $(TEST_SOURCES) $(HEADERS) | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_SOURCES) -o $@ $(ARCH_FLAGS) -pthread $(FS_LIBS)

build:
	mkdir -p build

test: $(TEST_TARGET)
	./$(TEST_TARGET)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/landrop
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 $(DESKTOP_FILE) \
		$(DESTDIR)$(PREFIX)/share/applications/landrop.desktop
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 $(ICON_FILE) \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/landrop.svg

clean:
	rm -rf build
