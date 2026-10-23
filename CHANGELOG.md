# Changelog

## Unreleased

### Added

- GUI and TUI now share per-peer chat history keyed by IP address and nickname.
  Reconnecting to the same peer restores the 20 newest earlier messages in
  italics, followed by an `Earlier messages` divider. Message timestamps are
  saved with the history and shown as `[HH:mm]` in both interfaces.
- The Windows executable now embeds the LANChat app icon as a native Windows
  resource.
- Pushing a `vX.Y.Z` tag now builds a Windows x64 release and publishes both a
  portable ZIP and an MSI installer to a GitHub Release.
- GUI and TUI discovery now probe private IPv4 subnets with unicast UDP, so
  peers can be found without relying on broadcast delivery. Probes use the
  discovery port and peers advertise their TCP chat port (45454 by default).
  The host list excludes the app's own discovery ID and local interface
  addresses.
- Launching from an interactive terminal now selects TUI automatically;
  `--tui` remains available to force it.

### Changed

- The TUI shows connection tabs with peer names and addresses in the chat
  header. Press `Tab` to focus the tabs, use the arrow keys to choose one, and
  press `Enter` to activate it. `Tab` also focuses the top menu; use the arrow
  keys to choose `Chat` or `Settings`, then press `Enter` to open it. The host
  list heading is `HOSTS ON NETWORK`. The active tab has a cyan `*`; the tab
  currently being selected has a yellow `>` marker and highlight.
- The TUI labels outgoing messages as `You` to match the GUI.
- GUI and TUI connection tabs now show the remote IP for IPv4-mapped IPv6
  sockets, retain the address after disconnect, and label disconnected peers as
  `Nickname (Disconnected)`.
- The TUI reuses a disconnected tab when the same IP and nickname reconnect.
  `Ctrl-W` closes the active chat tab and disconnects it.
- Disconnected chat inputs are disabled and show an italic reconnect prompt in
  both GUI and TUI; messages cannot be sent until the connection returns.
- Tabs with messages received in another conversation are highlighted until
  selected. The GUI's default font size is now 11 pt.
- Chat messages are sent with Enter; the Send button has been removed from the
  chat input row.
- The GUI saves open chat tabs and restores them on launch. Disconnected peers
  are retried every 10 seconds while their tab remains open.
- Host discovery always scans `10.8.0.1–10.8.0.255` and
  `10.10.10.1–10.10.10.255`, even when those subnets are not local interfaces.
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

- The GUI chat transcript now records when a peer connects or disconnects.
- The TUI no longer prints the connection confirmation twice.
- The tray's unread badge clears when LANChat becomes the active window and
  does not increment for messages received while the app is in the foreground.
