# LANChat

LANChat is a Qt 6 desktop client for direct LAN chat using the TCP-based
`LANCHAT PROTOCOL`. Each connected host has its own chat tab. The optional
Live Translate feature uses a local Ollama `translategemma` model.
The desktop app includes a custom chat icon and a system tray icon that shows
unread messages received while LANChat is in the background.

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
tabs, and a message area. Press `Tab` to move between the host list, chat, and
menu; use arrow keys to navigate and `Enter` to connect, select, or send.
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
Images are displayed in the chat log; received files are stored in the
`LANChat` folder under the operating system's Downloads location.

## Source layout

- `protocol/chatsession.*` implements the TCP LANCHAT protocol, file
  transfers, and Live Translate negotiation.
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
