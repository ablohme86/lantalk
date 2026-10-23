#!/usr/bin/env bash
# Runs inside the packaging/ci container, from the repository root.
#
#   linux-build.sh test      builds LANTalk with its unit tests and runs them
#   linux-build.sh package   builds a release and writes dist/$PACKAGE.tar.gz
set -euo pipefail

mode="${1:-}"
case "$mode" in
  test)
    cmake -S . -B build-ci-test -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-ci-test
    ctest --test-dir build-ci-test --output-on-failure
    ;;
  package)
    : "${PACKAGE:?set PACKAGE to the package name, e.g. LanTalk-v1.2.0-linux-x86_64}"
    rm -rf build-ci-release "dist/$PACKAGE" "dist/$PACKAGE.tar.gz"
    cmake -S . -B build-ci-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DLANTALK_BUILD_TESTS=OFF
    cmake --build build-ci-release
    # Qt's deploy step requires an absolute install prefix.
    cmake --install build-ci-release --prefix "$PWD/dist/$PACKAGE"
    test -x "dist/$PACKAGE/bin/lantalk"
    test -x "dist/$PACKAGE/bin/lantalk-centralized-service"
    cp README.md LICENSE CHANGELOG.md "dist/$PACKAGE/"
    tar -C dist -czf "dist/$PACKAGE.tar.gz" "$PACKAGE"
    ;;
  *)
    echo "usage: linux-build.sh test|package" >&2
    exit 2
    ;;
esac
