WINDOWS_BUILD_DIR ?= build-windows
WINDOWS_QT_PREFIX ?=
WINDOWS_TOOLCHAIN_FILE ?=
LANTALKD_BUILD_DIR ?= build-lantalkd
PREFIX ?= /usr/local
SYSCONFDIR ?= /etc
SYSTEMD_UNIT_DIR ?= /etc/systemd/system

.PHONY: build build-windows lantalkd install

build:
	cmake -S . -B build
	cmake --build build

# Builds only lantalkd, the central host, which needs just Qt Core and Network.
lantalkd:
	cmake -S . -B $(LANTALKD_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DLANTALK_BUILD_CLIENT=OFF
	cmake --build $(LANTALKD_BUILD_DIR)

# Installs lantalkd and its systemd unit; run "make lantalkd" first. An
# existing lantalkd.conf is kept, since it holds the password.
install:
	@test -x $(LANTALKD_BUILD_DIR)/lantalkd || (echo "Run make lantalkd first."; exit 1)
	install -D -m 755 $(LANTALKD_BUILD_DIR)/lantalkd $(DESTDIR)$(PREFIX)/bin/lantalkd
	install -d -m 755 $(DESTDIR)$(SYSTEMD_UNIT_DIR)
	sed -e 's|@BINDIR@|$(PREFIX)/bin|' -e 's|@SYSCONFDIR@|$(SYSCONFDIR)|' \
		packaging/systemd/lantalkd.service.in > $(DESTDIR)$(SYSTEMD_UNIT_DIR)/lantalkd.service
	chmod 644 $(DESTDIR)$(SYSTEMD_UNIT_DIR)/lantalkd.service
	install -d -m 755 $(DESTDIR)$(SYSCONFDIR)/lantalk
	test -e $(DESTDIR)$(SYSCONFDIR)/lantalk/lantalkd.conf || \
		install -m 600 packaging/systemd/lantalkd.conf $(DESTDIR)$(SYSCONFDIR)/lantalk/lantalkd.conf
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null; then systemctl daemon-reload; fi
	@echo "Set the password in $(SYSCONFDIR)/lantalk/lantalkd.conf, then run:"
	@echo "  sudo systemctl enable --now lantalkd"

# Requires a static Qt 6 MinGW kit. Set WINDOWS_QT_PREFIX to that kit's prefix
# and, when cross-compiling from Linux, WINDOWS_TOOLCHAIN_FILE to its toolchain.
build-windows:
	@test -n "$(WINDOWS_QT_PREFIX)" || (echo "Set WINDOWS_QT_PREFIX to a static Qt 6 MinGW kit."; exit 1)
	cmake -S . -B $(WINDOWS_BUILD_DIR) -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_PREFIX_PATH="$(WINDOWS_QT_PREFIX)" \
		-DCMAKE_TOOLCHAIN_FILE="$(WINDOWS_TOOLCHAIN_FILE)"
	cmake --build $(WINDOWS_BUILD_DIR)
