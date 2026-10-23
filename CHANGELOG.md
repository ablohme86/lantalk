# Changelog

## Unreleased

### Added

- GUI and TUI discovery now probe private IPv4 subnets with unicast UDP, so
  peers can be found without relying on broadcast delivery. Probes use the
  discovery port and peers advertise their TCP chat port (45454 by default).
  The host list excludes the app's own discovery ID and local interface
  addresses.
- Launching from an interactive terminal now selects TUI automatically;
  `--tui` remains available to force it.

### Fixed

- The TUI no longer prints the connection confirmation twice.
