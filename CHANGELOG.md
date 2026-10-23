# Changelog

## Unreleased

### Added

- Quotes: pasting several lines or a long text adds it as a quote, shown as a
  card above the GUI input or before the TUI input. Quotes are sent as
  `[quote]` blocks after the typed text, shown as shaded (GUI) or `│`-marked
  (TUI) blocks, and skipped by Live Translate unless "Translate this" is
  ticked (TUI: `/quote-translate`; `/unquote` removes a quote). `Ctrl+Shift+V`
  pastes plain text in the GUI.
- The TUI uses bracketed paste, so a multi-line paste no longer sends each
  line as a separate message.

## v1.2.0 - 2026-10-23

### Added

- The GUI message input is a multi-line editor: `Shift+Enter` starts a new
  line, pasted code keeps its line breaks and indentation, and the editor
  grows up to six lines. The TUI shows multi-line messages on indented lines.
- The GUI window title and the TUI menu bar show the version
  (`LANTalk vX.Y.Z`).
- The Windows MSI adds Start menu and desktop shortcuts with the LANTalk
  icon; pass `DESKTOPSHORTCUT=0` to skip the desktop shortcut.

### Fixed

- The Windows MSI and ZIP of v1.1.0 left out Qt's `plugins` folder, so the
  app failed with "no Qt platform plugin could be initialized". The packages
  now keep Qt's install layout (`bin\lantalk.exe` next to `plugins\`), and
  the release fails if the platform plugin is missing.
- The MSI installs into `C:\Program Files\LANTalk` instead of a folder named
  after both the manufacturer and the product.

### Changed

- The typing notice reads `Name is typing a message...` and appears in the
  TUI status line above the input instead of the chat pane's border.

- Releases are built on self-hosted runners: Linux in a podman container
  (`packaging/ci`), Windows on a dedicated machine. Packages are uploaded
  straight into a draft release that is published once all are present.

- The app is renamed from LANChat to LANTalk, matching the repository. The
  executable is now `lantalk` and release files are named `LanTalk-...`. On
  first launch LANTalk copies the settings, history, and open tabs saved by
  LANChat. The wire protocol is unchanged, so LANTalk and LANChat discover
  and chat with each other, and the Windows installer replaces an installed
  LANChat.

## v1.1.0 - 2026-10-23

### Added

- Live Translate languages are saved per conversation and restored after
  reconnects and restarts. Translation resumes automatically once Ollama is
  reachable with the configured model, retrying while it is unavailable.
- Connecting to a peer that already has a conversation switches to its tab in
  both GUI and TUI. Only one connection per IP address and nickname can exist;
  reconnects and simultaneous connections are merged into the existing tab.
- A `Name is typing...` indicator in GUI and TUI, using the new
  `CAPABILITIES TYPING`, `TYPING START`, and `TYPING STOP` protocol lines.
  Drafts idle for 60 seconds send `TYPING STOP`; stale indicators expire.
- Unit tests for typing notifications, duplicate-connection handling, and
  translation restoration (`ctest`).

- GUI and TUI now share per-peer chat history keyed by IP address and nickname.
  Reconnecting to the same peer restores the 20 newest earlier messages in
  italics, followed by an `Earlier messages` divider. Message timestamps are
  saved with the history and shown as `[HH:mm]` in both interfaces.
- The Windows executable now embeds the LANChat app icon as a native Windows
  resource.
- The Release workflow can be run manually to bump the minor (or patch/major)
  version, tag it, run the unit tests, and publish a GitHub Release with a
  Linux x86_64 tarball, a Windows MSI installer, and a portable Windows ZIP.
  Pushing a `vX.Y.Z` tag publishes that version without bumping it.
- GUI and TUI discovery now probe private IPv4 subnets with unicast UDP, so
  peers can be found without relying on broadcast delivery. Probes use the
  discovery port and peers advertise their TCP chat port (45454 by default).
  The host list excludes the app's own discovery ID and local interface
  addresses.
- Launching from an interactive terminal now selects TUI automatically;
  `--tui` remains available to force it.

### Changed

- The TUI menu bar now looks like MS-DOS Edit: a blue bar with underlined
  accelerator letters and drop-down menus with keyboard shortcuts. `Down`
  opens the focused menu, and accelerator letters pick menus and items.
- The TUI input stays usable while the active conversation is disconnected,
  so slash commands such as `/connect` work; messages are not sent and the
  reconnect prompt appears in the status line instead.
- The GUI opens a tab for an incoming connection only after its handshake, so
  duplicate or aborted connections no longer leave stray tabs.

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
