#!/usr/bin/env bash
# Prints the CHANGELOG section for a version (e.g. v1.1.0), or nothing if the
# changelog has no section for it.
#
# Usage: scripts/release-notes.sh vX.Y.Z
set -euo pipefail

tag="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

awk -v heading="## $tag" '
  index($0, heading) == 1 { printing = 1; next }
  printing && /^## / { exit }
  printing { print }
' "$root/CHANGELOG.md" | sed -e '/./,$!d'
