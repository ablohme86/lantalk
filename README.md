# LANTalk

LANTalk (formerly LANChat) is a Qt 6 desktop client for direct LAN chat using
the TCP-based `LANCHAT PROTOCOL`, so it interoperates with LANChat installs.
On first launch it copies the settings, chat history, and open tabs saved by
LANChat. Each connected host has its own chat tab. The optional
Live Translate feature uses a local Ollama `translategemma` model.
The desktop app includes a custom chat icon and a system tray icon that shows
unread messages received while LANTalk is in the background. Messages and
files that arrive while the window is not active also raise a desktop
notification (a toast on Windows, the desktop's notification service on
Linux) with the sender and a short preview (translated, when Live Translate
translates the message); clicking it opens that conversation. Turn it off under Settings → General → Notifications. The GUI remembers
open chat tabs across restarts and retries disconnected peers every 10 seconds
while their tab remains open.
Connection tabs show each peer's IP address while connected and use
`Nickname (Disconnected)` after the peer disconnects.
Disconnected conversations reject new messages until the connection returns.
The GUI disables the input and shows an italic reconnect prompt; the TUI keeps
its input usable for slash commands such as `/connect` and shows the reconnect
prompt in the status line above it.
The GUI shows each conversation as chat bubbles: received messages on the
left in gray, your messages on the right in blue, with the local `HH:mm` time
under each bubble (the TUI shows `[HH:mm]` timestamps). Tabs highlight when another
conversation receives a message and clear the highlight when selected.
The GUI starts with an 11 pt font; change it under Settings → Font Size.

LANTalk keeps at most one connection per peer (IP address + nickname).
Connecting to a host you already have a conversation with, from either the GUI
or the TUI, switches to its existing tab instead of opening a new connection;
a disconnected tab is retried immediately. The rule is enforced for every
connection, including incoming ones: when a peer reconnects, or both sides
connect at the same moment, the connections are merged into the existing tab
and its history and state are kept.

The GUI looks for Ollama servers on port 11434 on this computer and on the
private subnets of its network cards (up to a /22 each) at startup, every five
minutes, and after Settings are saved. Live Translate is offered when an
Ollama server is configured or one was found; its dialog lists the servers
found (plus the configured one), with a Rescan button and each server's
models, next to the language choices. The chosen server and model become the
Ollama settings.

Live Translate languages are remembered per conversation. After a reconnect
or an application restart, Live Translate turns itself back on as soon as the
configured Ollama server is reachable and has the configured model; while the
server is unavailable, LANTalk retries every 30 seconds. Turning Live
Translate off is remembered as well.

When the other participant is composing a message, `Name is typing a
message...` appears above the message input: as a line above the input in the
GUI, and in the status line in the TUI. A draft left untouched for 60 seconds
stops counting as typing. Both participants need LANTalk (or LANChat v1.1.0
or later) for typing notifications.

The GUI message input is a multi-line editor: `Enter` sends, `Shift+Enter`
starts a new line, and pasted text such as code keeps its line breaks and
indentation. It grows to twelve lines and scrolls beyond that. The TUI shows
multi-line messages with indented continuation lines.

Pasting several lines, or more than 200 characters, adds the text as a quote
instead: a card above the GUI input that shows the text in an editable field,
so it can be checked and changed before sending (in the TUI, `[quote 1: N
lines]` before the input). Quotes are sent after your typed text, shown as a
block, and never translated by Live Translate unless you tick its "Translate
quote" box, which appears while Live Translate is on (TUI: `/quote-translate
[n|all]`; `/unquote [n|all]` removes it). Each card has a remove button, and
emptying its text removes it too; in the TUI, Backspace on an empty input
removes the last quote. `Ctrl+Shift+V` in the GUI pastes as plain text. The
GUI window title and the TUI menu bar show the LANTalk version.

The app automatically selects the ANSI terminal interface when launched from
an interactive terminal. It uses the desktop GUI otherwise. Pass `--tui` to
force terminal mode. The TUI listens for chats, discovers LANTalk peers, and
supports multiple conversations:

```sh
lantalk --tui
```

On its first run, terminal mode asks for a nickname and stores it in the same
application settings used by the GUI. In a regular TTY it opens a full-screen
ANSI interface with `Chat` and `Settings` menu items, discovered hosts, chat
tabs labeled with each peer's name and address, and a message area. The host
list is labeled `HOSTS ON NETWORK`. Press `Tab` to move between the host list,
chat input, connection tabs, and menu. Use the arrow keys to navigate and
`Enter` to connect, activate a tab, open the focused top menu, or send. At the
top menu, left/right selects `Chat` or `Settings` before you press `Enter`.
GUI and TUI share chat history for peers with the same IP address and nickname.
When a conversation is reopened, its 20 newest earlier messages appear in
italics above an `Earlier messages` divider.
In the TUI, a reconnect from the same IP and nickname resumes the existing
disconnected tab; press `Ctrl-W` to close and disconnect the active chat.
The TUI has an MS-DOS Edit style menu bar. Press `Alt-C` for the Chat menu
(connect, close the active chat, or quit) and `Alt-S` for Settings (nickname,
Ollama URL/model, listening port, and central host). Menus drop down below the bar; use the
arrow keys or the underlined letter to pick an item. Menu confirmations use
keyboard-friendly ASCII dialogs; `Esc` cancels.
Typing and sending chat messages works while the chat area has focus. The
existing slash commands remain available (`/help`, `/hosts`, `/connect`,
`/tabs`, `/use`, `/close`, `/nick`, `/translate`, `/stoptranslate`, and
`/quit`) and are used as the fallback when input or output is not an
interactive TTY. Enable translation for the active conversation with
`/translate <my language> | <peer language>`; pending Ollama translations appear
in the bottom status line.

Available LANTalk hosts are discovered automatically and shown as
`Nickname (IP address)` in the read-only Host selector. LANTalk uses UDP
announcements and unicast probes to addresses in private IPv4 interface
subnets; peers answer with their configured TCP chat port (45454 by default).
Subnets wider than `/16` are skipped to avoid sweeping very large private
address ranges. LANTalk filters its own discovery ID and local interface
addresses from the host list. Selecting a discovered host connects immediately.

Drop images or files into a conversation to send them to the connected host.
Image thumbnails appear in the chat log and open in an in-chat viewer when
clicked. Received attachments are kept in LANTalk's operating-system cache
until that conversation is closed. Use the `Save as...` link beside any
attachment to copy it to a chosen location. File transfers show progress in the
chat input, and launching LANTalk again activates the running instance.

## Central host

LANTalk normally talks directly to peers on the LAN. When the computers
cannot reach each other directly (different networks, VPNs, firewalls), run
`lantalk-centralized-service` on a server that all of them can reach and
enter its IP address or DNS name, optionally with `:port`, under
**Settings → General → Central host** (TUI: **Settings → Central Host**).
In central mode LANTalk:

- stops listening on its TCP port and stops LAN discovery;
- lists the clients connected to the central host in the host selector
  (`HOSTS ON NETWORK` in the TUI), shown as `Nickname (IP address)` with the
  address the central host sees;
- sends every conversation through the central host. Everything else works
  as before: tabs, history, reconnects, typing, quotes, files, and Live
  Translate, which still translates locally with your own Ollama server.

Leave the field empty to go back to direct LAN chat. Changing it moves open
conversations to the new route at once. Conversations are identified by the
peer's address and nickname, so the history of a peer seen through a
central host is kept apart from the history of the same peer on the LAN if
the addresses differ.

The service is a separate console program built and installed next to
`lantalk` (`bin/lantalk-centralized-service`, Qt Core and Network only). It
runs on any Linux or Windows server:

```sh
lantalk-centralized-service --port 45455            # all addresses
lantalk-centralized-service --listen 10.0.0.5 -p 45455
```

To require a password, put it on the first line of a file and pass
`--password-file <file>`, or set the `LANTALK_CENTRAL_PASSWORD` environment
variable. There is no `--password` option because other users on the server
can read command lines. Clients enter the same password under **Settings →
General → Central host password** (TUI: the second prompt of **Settings →
Central Host**). The password never crosses the network: the service sends
a random challenge, and the client answers with an HMAC-SHA256 of it keyed
with the password. A wrong or missing password is refused after a
one-second delay and logged, and the client stops retrying until its
settings change. Clients keep the password in their LANTalk settings in
plain text.

It logs sign-ins, sign-outs, and refused sign-ins, never message contents.
A systemd unit might look like:

```ini
[Unit]
Description=LANTalk central host
After=network-online.target

[Service]
ExecStart=/opt/lantalk/bin/lantalk-centralized-service --port 45455 --password-file ${CREDENTIALS_DIRECTORY}/password
LoadCredential=password:/etc/lantalk/central-password
DynamicUser=yes
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Without a password, anyone who can reach the port can sign in under any
nickname and see who is online. The password only controls who signs in:
everyone who knows it can still use any nickname. Conversations are not
encrypted and cross the network in clear text, just like LAN chat, and
someone who records a sign-in can try to guess a weak password offline.
Use a long password, and run the service on a trusted network or behind a
VPN.

### Central protocol

Clients and the service exchange `CENTRAL_` lines over TCP. The service
opens with `CENTRAL_CHALLENGE <nonce>`. The client signs in with
`CENTRAL_HELLO <version> <nickname> <port> <proof>`, where the proof is the
hex HMAC-SHA256 of `LANTALK_CENTRAL <nonce>` keyed with the password, or
`-` without one. A refused sign-in gets `CENTRAL_REJECT <reason>`. A
signed-in client receives one
`CENTRAL_PEER <id> <nickname> <address> <port>` per connected client
followed by `CENTRAL_WELCOME <id> <address>`. After that the service reports
`CENTRAL_PEER` and `CENTRAL_PEER_GONE <id>` as clients come and go.
`CENTRAL_OPEN <channel> <peer id>` opens a conversation (the peer gets
`CENTRAL_INCOMING <channel> <id>` and the opener `CENTRAL_OPENED <channel>`).
`CENTRAL_DATA <channel> <line>` carries one unchanged LANCHAT PROTOCOL line,
and `CENTRAL_CLOSE <channel> [reason]` ends it. Nicknames are base64url
encoded. Clients send `CENTRAL_PING` every 30 seconds, and either side drops
a connection that is silent for 90 seconds. A new sign-in with the same
address and nickname replaces the old one, which receives
`CENTRAL_REPLACED`.

## Protocol extensions

LANCHAT PROTOCOL is line based. After receiving the peer's `NICKNAME`, current
clients send `CAPABILITIES TYPING`; unknown capability tokens are ignored.
Typing notifications are sent only to peers that advertised `TYPING`, so
older clients never receive them (they report the `CAPABILITIES` line once as
an unsupported command).

- `TYPING START` is sent when the local draft becomes non-empty and repeated
  every 15 seconds while the user keeps composing.
- `TYPING STOP` is sent when the draft is cleared or sent, after 60 seconds
  without edits, and when the conversation is closed.

Receivers hide the indicator on `TYPING STOP`, on an incoming `MESSAGE`, on
disconnect, or 35 seconds after the last `TYPING START`.

Quotes travel inside the message text as `[quote]` and `[/quote]` lines.
LANTalk renders them as blocks and does not translate them; clients without
quote support show the tags as text. The "Translate quote" choice is kept on
the sender's side and never sent.

## Source layout

- `protocol/chatsession.*` implements the TCP LANCHAT protocol, file
  transfers, typing notifications, and Live Translate negotiation.
- `protocol/chattransport.*` is the byte stream a session runs over (direct
  TCP or a relayed channel); `protocol/centralclient.*` connects to a
  central host and relays channels through it, and
  `protocol/centralprotocol.h` holds what client and service share.
- `central/` is `lantalk-centralized-service`.
- `protocol/connectionregistry.*` enforces one connection per peer, and
  `protocol/conversationidentity.*` defines how peers are compared.
- `translationrestorer.*` stores Live Translate languages per conversation and
  resumes translation after reconnects.
- `gui/dialogs.*`, `gui/chattab.*`, and `gui/mainwindow.*` contain the Qt UI,
  including LAN host discovery.
- `tui/terminalui.*` provides the dependency-free ANSI terminal UI, sharing
  the protocol, settings, TCP listener, and LAN discovery format with the GUI.
- `main.cpp` selects the GUI by default or terminal UI with `--tui`.
- `appconfig.h` and `ollamaclient.*` provide shared configuration and the
  optional Ollama translation client.

## Build

Install Qt 6 with the **Widgets** and **Network** components, then run:

```sh
cmake -S . -B build
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

Run the unit tests with `ctest --test-dir build` (requires Qt's Test module;
configure with `-DLANTALK_BUILD_TESTS=OFF` to skip them).

On Linux, run `dist/bin/lantalk`. On Windows, run `dist/bin/lantalk.exe`.
The install step invokes Qt's deployment support, which places the Qt runtime
files needed by the executable in the install directory.

### Windows

Build from a Qt-provided developer command prompt, or ensure that CMake can
find the matching Qt kit:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

Use the Qt kit that matches the selected compiler (for example, an MSVC kit
with Visual Studio or a MinGW kit with MinGW). The resulting `dist` directory
can be copied to another compatible Windows computer.

## Releases

Releases are published on the repository's GitHub **Releases** page by the
`Release` workflow. Start it from **Actions → Release → Run workflow** on the
default branch and choose which part of the version to bump (`minor` by
default). The workflow runs the unit tests, updates the version in
`CMakeLists.txt`, moves the `Unreleased` changelog entries under the new
version, commits and tags `vX.Y.Z`, and publishes:

- `LanTalk-vX.Y.Z-linux-x86_64.tar.gz`: extract it anywhere and run
  `bin/lantalk`. Qt is bundled; the desktop GUI needs the X11 libraries
  `libxcb-cursor0` and `libxkbcommon-x11-0` (on Wayland it runs through
  XWayland).
- `LanTalk-vX.Y.Z-windows-x64.msi` installs into `C:\Program Files\LANTalk`
  and adds Start menu and desktop shortcuts (`msiexec /i <msi>
  DESKTOPSHORTCUT=0` skips the desktop one).
- `LanTalk-vX.Y.Z-windows-x64.zip` is portable: extract it and run
  `bin\lantalk.exe`.

Pushing a `vX.Y.Z` tag yourself publishes that version without bumping it. The
release notes come from the matching `CHANGELOG.md` section plus GitHub's
generated notes.

The workflow runs on two self-hosted runners registered for this repository:
a Linux machine with podman (label `lantalk-linux`), which tests and builds
inside the `packaging/ci/Containerfile` image, and a Windows machine (label
`lantalk-windows`) that builds the MSI and ZIP. Register them with
`packaging/ci/setup-linux-runner.sh` and `packaging/ci/setup-windows-runner.ps1`,
using a token from
`gh api -X POST repos/ablohme86/lantalk/actions/runners/registration-token -q .token`.
