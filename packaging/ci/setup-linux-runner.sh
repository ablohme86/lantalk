#!/usr/bin/env bash
# Registers a GitHub Actions runner for lantalk (label lantalk-linux) on a
# Linux machine that already has podman, git and gh, such as the one running
# allglider's runner. It lives in its own folder and runs as its own systemd
# service, next to allglider's.
#
#   packaging/ci/setup-linux-runner.sh <registration token>
#
# The token is the repository's runner registration token (valid for an hour):
#   gh api -X POST repos/ablohme86/lantalk/actions/runners/registration-token -q .token
#
# REPO, RUNNER_DIR, NAME and LABELS override the defaults below. Run it as the
# user the runner should build as (not root); it uses sudo for the service.
set -euo pipefail

token="${1:?usage: setup-linux-runner.sh <registration token>}"
repo="${REPO:-ablohme86/lantalk}"
runner_dir="${RUNNER_DIR:-$HOME/actions-runner-lantalk}"
name="${NAME:-$(hostname)-lantalk}"
labels="${LABELS:-lantalk-linux}"

for tool in podman git gh curl tar; do
  command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

mkdir -p "$runner_dir"
cd "$runner_dir"

if [[ ! -x ./config.sh ]]; then
  version="$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')"
  echo "==> Downloading runner $version"
  curl -fsSL "https://github.com/actions/runner/releases/download/v$version/actions-runner-linux-x64-$version.tar.gz" | tar -xz
fi

echo "==> Registering $name ($labels) for $repo"
./config.sh --unattended --url "https://github.com/$repo" --token "$token" \
  --name "$name" --labels "$labels" --work _work --replace

echo "==> Installing the service"
sudo ./svc.sh install "$USER"
sudo ./svc.sh start
sudo ./svc.sh status | head -5
echo "Done: the runner '$name' ($labels) is waiting for jobs."
