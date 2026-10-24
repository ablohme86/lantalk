# Changelog

## Unreleased

## v1.14.0 - 2026-10-24

### Added

- Personal PIN: the first time a nickname signs in to a central host,
  LANTalk, the TUI, and the web client offer to protect it with a 4-digit
  PIN. With it, the same nickname can be signed in on several devices at
  once: others see one person, every device gets the conversation, and what
  one device sends shows up on the others as sent (files and Live Translate
  included). Nobody can sign in with the nickname without the PIN; five
  wrong ones lock it for a while. **Settings → Personal PIN...**, `/pin`, and
  the web client's lock button set or change it later.
- `lantalkd` keeps the history of every conversation a PIN-protected
  nickname has, in its data folder (`--data-dir`, `/var/lib/lantalkd` under
  systemd), so a device that was off or new catches up when it signs in.
  `--remove-pin <nickname>` removes a forgotten PIN, `--list-pins` lists
  them.

## v1.13.0 - 2026-10-24

### Added

- Away status: after 10 minutes without touching LANTalk (keyboard or mouse
  in the GUI, a key in the TUI, a touch or key on the web page), others see
  you with a yellow dot instead of a green one, on conversation tabs, in the
  Hosts sidebar, and on the web client (`(away)` in the TUI). It travels as
  `PRESENCE AWAY|ACTIVE` in conversations with peers that support it and
  through `lantalkd` for its host list.

### Changed

- Files up to 1 GB can be sent (was 50 MB), in LANTalk and on the web
  client; `lantalkd --max-file-size` can still lower that. Received files
  are written to disk as they arrive and the web client reads files in
  pieces, so large files no longer sit in memory. LANTalk tells peers that
  it takes large files (`CAPABILITIES LARGEFILES`) and sends at most 50 MB
  to older versions, saying why.
- Through a central host, a dropped conversation reconnects as soon as
  `lantalkd` reports the peer online again, instead of retrying every 10
  seconds; the status says it is waiting for them. The TUI, which did not
  reconnect by itself before, does this too, and the web client reopens
  the conversation in view.
- `lantalkd` paces relayed conversations: while a receiver has more than a
  few megabytes waiting, it stops reading from the sender, so TCP slows the
  sender down instead of `lantalkd` buffering whole files or dropping the
  conversation of a slow receiver.

## v1.12.0 - 2026-10-24

### Changed

- The GUI lists the available hosts in a **Hosts** sidebar, as the TUI and
  the web client do, instead of a drop-down list: nickname with the address
  (or `Web`) below it, one click to open or switch to the conversation, and
  the open conversation's host selected. The sidebar's width is remembered.
- Conversation tabs show a green dot while the conversation is connected
  and a red one while it is not (connecting, reconnecting, or
  disconnected).

## v1.11.0 - 2026-10-24

### Added

- LANTalk (GUI and TUI) uses Live Translate through the central host when
  `lantalkd` has an Ollama server configured (`--ollama-url`,
  `--ollama-model`) that answers with its model; no Ollama server of your
  own is needed, and the dialog only asks for the languages. `lantalkd`
  checks that server every minute and logs when it becomes available or
  stops answering.

## v1.10.0 - 2026-10-24

### Added

- A soft chime plays for new messages that would otherwise go unseen (the
  window is in the background or another tab is open), at most once per
  burst. **Settings → General** can turn it off. It uses Qt Multimedia
  when available and the system beep otherwise.
- The GUI is available in English, Norwegian, Simplified Chinese, and
  Traditional Chinese. It follows the system language (English otherwise),
  and **Settings → General → Language** overrides it after a restart.
- `lantalkd` serves a web client for phones and browsers on port 45456
  (`--web-port`, `0` turns it off; `--web-cert`/`--web-key` for HTTPS).
  People sign in with a nickname and the central host password and chat
  with LANTalk users: people list with search and unread counts, typing,
  quotes, links, images, files, history in the browser, and messages kept
  for people who are away. It follows the browser's language and dark mode.
- Live Translate on the web client: `lantalkd` translates with the Ollama
  server given by `--ollama-url`/`--ollama-model`, or the page offers the
  Ollama servers `lantalkd` finds on its network (`--no-ollama-discovery`
  turns the search off) and an Ollama address of the user's own.
- LANTalk (GUI and TUI) shows web users as `Nickname (Web)` in the host
  list and on their tabs.
- File transfers show as a bubble on both sides while they travel
  ("Uploading <file> to <person>. 42%…" / "Receiving <file> from
  <person>. 42%…"), which becomes the attachment when it arrives, in
  LANTalk and on the web client. The chat input no longer shows the
  progress and stays usable during an upload.
- `lantalkd --max-file-size <MB>` limits the files it relays (default 50;
  0 relays none). Clients refuse larger files at once, and `lantalkd`
  stops them from older clients.
- Chat history keeps every message instead of the last 500, in a file per
  conversation (existing history moves there by itself). A reopened
  conversation shows the last 200, and **Show earlier messages** loads 200
  more at a time. The web client keeps up to 1000 messages per
  conversation in the browser.

### Changed

- While the other person has Live Translate on, your Live Translate button
  is disabled and says why, so only one side translates at a time.

### Fixed

- Messages LANTalk translated with Live Translate came back untranslated
  from the history after a restart; the history now keeps the translation
  and the original (GUI and TUI).

## v1.9.0 - 2026-10-24

### Changed

- The central host service is now called `lantalkd` (it was
  `lantalk-centralized-service`). On Linux, `make lantalkd` builds only the
  service and `sudo make install` installs it with a systemd unit and
  `/etc/lantalk/lantalkd.conf` for the password and options.
- In central host mode, LANTalk looks for Ollama only on this computer and
  at the configured URL instead of scanning the network cards' subnets,
  which may be VPN adapters that accept every connection.

### Fixed

- The main window appears before the nickname prompt and before LANTalk
  starts listening and scanning, so a slow network (for example with a VPN)
  no longer leaves it without a window.
- Starting LANTalk while a hung copy is still running showed nothing and
  exited; it now says that LANTalk is running but not responding and how
  to end it.
- LAN discovery reads this computer's addresses at most every five seconds
  instead of for every discovery message, which was slow on Windows with
  VPN adapters.

## v1.8.0 - 2026-10-24

### Added

- Central host mode. Enter the IP address or DNS name of a server running
  the new `lantalk-centralized-service` (optionally with `:port`, default
  45455) under Settings → General → Central host, or Settings → Central
  Host in the TUI. LANTalk then stops listening for chats and scanning the
  LAN. The host list shows the clients connected to the central host, and
  every conversation, including typing notifications, files and the Live
  Translate handshake, is relayed through it. Translation itself still
  happens locally with your own Ollama server. Start the service with
  `--password-file <file>` (or `LANTALK_CENTRAL_PASSWORD`) to require a
  password, which clients enter as the Central host password; it is
  checked by challenge-response and never sent over the network. Clearing the field returns
  to direct LAN chat, and open conversations move to the new route right
  away.

## v1.7.0 - 2026-10-23

### Added

- The GUI finds Ollama servers on port 11434 on this computer and on the
  network cards' private subnets (up to a /22 each), at startup, every five
  minutes, and after Settings are saved. The Live Translate button appears
  when a server is found or configured, and its dialog offers the servers
  found and their models, with a Rescan button, alongside the languages.

## v1.6.0 - 2026-10-23

### Changed

- The GUI shows conversations as chat bubbles: received messages on the
  left in gray, sent messages on the right in blue, each with its time
  below. Quotes appear as monospace blocks inside the bubble, translations
  replace the bubble's text with the original in its tooltip, and
  connection notices and the history divider are centred between the
  bubbles. Files appear as bubbles with a `Save as...` link and image
  thumbnail.

## v1.5.0 - 2026-10-23

### Added

- Desktop notifications for messages and files that arrive while the GUI
  window is not active: a toast on Windows and a notification through the
  desktop's notification service on Linux, showing the sender and a short
  preview. With Live Translate on, the notification waits for the
  translation and shows it. Clicking one opens that conversation. They can
  be turned off in Settings.

### Changed

- The quote checkbox reads "Translate quote" and appears only while Live
  Translate is on. Deleting all of a quote's text removes the quote.
- When the other participant turns on Live Translate, the status shows the
  language pair from your side, your language first (for example
  `English → Norwegian Bokmål` for the English-speaking participant).

## v1.4.0 - 2026-10-23

### Added

- GUI quote cards show the pasted text in an editable field that grows to
  twelve lines, so a quote can be reviewed and changed before it is sent.
  The message input also grows to twelve lines instead of six.

## v1.3.0 - 2026-10-23

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
