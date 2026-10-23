WINDOWS_BUILD_DIR ?= build-windows
WINDOWS_QT_PREFIX ?=
WINDOWS_TOOLCHAIN_FILE ?=

.PHONY: build build-windows

build:
	cmake -S . -B build
	cmake --build build

# Requires a static Qt 6 MinGW kit. Set WINDOWS_QT_PREFIX to that kit's prefix
# and, when cross-compiling from Linux, WINDOWS_TOOLCHAIN_FILE to its toolchain.
build-windows:
	@test -n "$(WINDOWS_QT_PREFIX)" || (echo "Set WINDOWS_QT_PREFIX to a static Qt 6 MinGW kit."; exit 1)
	cmake -S . -B $(WINDOWS_BUILD_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$(WINDOWS_QT_PREFIX)" \
		-DCMAKE_TOOLCHAIN_FILE="$(WINDOWS_TOOLCHAIN_FILE)"
	cmake --build $(WINDOWS_BUILD_DIR)
