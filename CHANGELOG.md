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

### Changed

- The GUI menu bar now has a File menu with Exit LanChat and a Connect... action
  that opens the connection dialog.
- GUI attachments always start on a new line. Image thumbnails appear in the
  conversation and open in a full-view chat panel with a close button when
  clicked.
- Chat windows have an attachment icon button that opens a file picker to send
  a file.
- The GUI Settings menu has a persistent font size selector.
- Enabling Live Translate translates earlier incoming messages that have no
  translation yet.
- File sends are asynchronous and show italic progress in the chat input.
- Received attachments are stored in the OS cache, removed when their chat
  closes, and include a `Save as...` link. Sent attachments show only the
  filename, and outgoing messages use the `You` label.
- Live Translate status messages identify the peer who enabled or disabled it.
- The GUI host list shows `Scanning for hosts...` until its initial network scan
  completes.
- Previewing an image no longer constrains the chat window's resize range.
- Starting LANChat again activates the existing process instead of opening a
  second UI.

### Fixed

- The TUI no longer prints the connection confirmation twice.
- The tray's unread badge clears when LANChat becomes the active window and
  does not increment for messages received while the app is in the foreground.
