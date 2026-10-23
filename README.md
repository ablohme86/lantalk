# LANChat

LANChat is a Qt 6 desktop client for direct LAN chat using the TCP-based
`LANCHAT PROTOCOL`. Each connected host has its own chat tab. The optional
Live Translate feature uses a local Ollama `translategemma` model.
The desktop app includes a custom chat icon and a system tray icon that shows
unread messages received while LANChat is in the background. The GUI remembers
open chat tabs across restarts and retries disconnected peers every 10 seconds
while their tab remains open.
Connection tabs show each peer's IP address while connected and use
`Nickname (Disconnected)` after the peer disconnects.
Disconnected chat inputs show an italic reconnect prompt and reject new
messages until the connection returns.
Messages show local `[HH:mm]` timestamps. Tabs highlight when another
conversation receives a message and clear the highlight when selected.
The GUI starts with an 11 pt font; change it under Settings → Font Size.

LANChat keeps at most one connection per peer (IP address + nickname).
Connecting to a host you already have a conversation with, from either the GUI
or the TUI, switches to its existing tab instead of opening a new connection;
a disconnected tab is retried immediately. The rule is enforced for every
connection, including incoming ones: when a peer reconnects, or both sides
connect at the same moment, the connections are merged into the existing tab
and its history and state are kept.

Live Translate languages are remembered per conversation. After a reconnect
or an application restart, Live Translate turns itself back on as soon as the
configured Ollama server is reachable and has the configured model; while the
server is unavailable, LANChat retries every 30 seconds. Turning Live
Translate off is remembered as well.

When the other participant is composing a message, `Name is typing...` appears
above the message input (in the GUI) or in the chat pane's bottom border (in
the TUI). A draft left untouched for 60 seconds stops counting as typing.

The app automatically selects the ANSI terminal interface when launched from
an interactive terminal. It uses the desktop GUI otherwise. Pass `--tui` to
force terminal mode. The TUI listens for chats, discovers LANChat peers, and
supports multiple conversations:

```sh
lanchat --tui
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
Press `Alt-C` for the Chat menu (connect, close the active chat, or quit) and
`Alt-S` for Settings (nickname, Ollama URL/model, and listening port). Menus
and their confirmations use keyboard-friendly ASCII dialogs; `Esc` cancels.
Typing and sending chat messages works while the chat area has focus. The
existing slash commands remain available (`/help`, `/hosts`, `/connect`,
`/tabs`, `/use`, `/close`, `/nick`, `/translate`, `/stoptranslate`, and
`/quit`) and are used as the fallback when input or output is not an
interactive TTY. Enable translation for the active conversation with
`/translate <my language> | <peer language>`; pending Ollama translations appear
in the bottom status line.

Available LANChat hosts are discovered automatically and shown as
`Nickname (IP address)` in the read-only Host selector. LANChat uses UDP
announcements and unicast probes to addresses in private IPv4 interface
subnets; peers answer with their configured TCP chat port (45454 by default).
Subnets wider than `/16` are skipped to avoid sweeping very large private
address ranges. LANChat filters its own discovery ID and local interface
addresses from the host list. Selecting a discovered host connects immediately.

Drop images or files into a conversation to send them to the connected host.
Image thumbnails appear in the chat log and open in an in-chat viewer when
clicked. Received attachments are kept in LANChat's operating-system cache
until that conversation is closed. Use the `Save as...` link beside any
attachment to copy it to a chosen location. File transfers show progress in the
chat input, and launching LANChat again activates the running instance.

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

## Source layout

- `protocol/chatsession.*` implements the TCP LANCHAT protocol, file
  transfers, typing notifications, and Live Translate negotiation.
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
configure with `-DLANCHAT_BUILD_TESTS=OFF` to skip them).

On Linux, run `dist/bin/lanchat`. On Windows, run `dist/bin/lanchat.exe`.
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
