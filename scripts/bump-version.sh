#!/usr/bin/env bash
# Bumps the project version in CMakeLists.txt and dates the CHANGELOG's
# Unreleased section. Prints the new version (MAJOR.MINOR.PATCH).
#
# Usage: scripts/bump-version.sh [major|minor|patch]   (default: minor)
set -euo pipefail

part="${1:-minor}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_file="$root/CMakeLists.txt"
changelog="$root/CHANGELOG.md"

current="$(sed -nE 's/^project\(LanTalk VERSION ([0-9]+(\.[0-9]+){1,2}).*/\1/p' "$cmake_file")"
if [[ -z "$current" ]]; then
  echo "Cannot find 'project(LanTalk VERSION x.y[.z])' in CMakeLists.txt" >&2
  exit 1
fi
IFS=. read -r major minor patch <<<"$current"
patch="${patch:-0}"

case "$part" in
  major) major=$((major + 1)); minor=0; patch=0 ;;
  minor) minor=$((minor + 1)); patch=0 ;;
  patch) patch=$((patch + 1)) ;;
  *) echo "Unknown version part '$part'; use major, minor, or patch." >&2; exit 1 ;;
esac
next="$major.$minor.$patch"

sed -i -E "s/^project\(LanTalk VERSION [0-9.]+/project(LanTalk VERSION $next/" "$cmake_file"

# Keep an empty Unreleased section on top and move its entries under the
# new version heading.
if grep -q '^## Unreleased' "$changelog"; then
  sed -i "s/^## Unreleased\$/## Unreleased\n\n## v$next - $(date -u +%Y-%m-%d)/" "$changelog"
fi

echo "$next"
