# WinUAE + mcpbridge

A private fork of [tonioni/WinUAE](https://github.com/tonioni/WinUAE) (based on
tag `6030`) that embeds **mcpbridge** — an MCP / JSON-RPC server inside
`winuae64.exe`. It lets an MCP client (Claude Code, Claude Desktop, IDE
plugins) drive and inspect a running Amiga: synthesize mouse/keyboard input,
control the machine, read/write guest memory, set breakpoints, capture
screenshots and audio, talk to the emulated serial port, and more.

- The emulator listens on a TCP socket and speaks newline-delimited JSON-RPC.
- A tiny Python relay bridges the MCP client's stdio to that socket.
- The relay is resilient: start the client and emulator in **any order**,
  restart the emulator freely, and the MCP server stays "connected".

For the full design, rationale, and per-tool implementation notes, see
[`MCPBRIDGE_PROGRESS.md`](MCPBRIDGE_PROGRESS.md).

---

## 1. Prerequisites

| Need | Notes |
|---|---|
| Windows 10/11 64-bit | |
| Visual Studio 2022 (any edition) | C++ desktop workload; toolset **v143** |
| Windows SDK 10.0.26100 | the one bundling the WDK `km/` headers used here |
| NASM | `winget install NASM.NASM` → `C:\Program Files\NASM\nasm.exe` |
| Python 3.8+ | for the relay (the `py` launcher is assumed below) |
| WinUAE dev libs/includes | the `winuaeinclibs` bundle (see below) |
| A Kickstart ROM + a bootable Amiga disk/HDF | to actually run a guest |

### Dev libs / includes

The project files reference `C:\dev\include` and `C:\dev\lib\{x86,x64}`. This
repo ships the bundle under `dev/`. Make it visible at `C:\dev` with
directory junctions (no copy, no admin needed for these paths):

```powershell
New-Item -ItemType Junction -Path C:\Dev\include -Target C:\Dev\source\WinUAE\dev\include
New-Item -ItemType Junction -Path C:\Dev\lib     -Target C:\Dev\source\WinUAE\dev\lib
```

(Windows path lookups are case-insensitive, so `C:\dev\...` resolves through
`C:\Dev\...`.)

---

## 2. Build

The project files in this fork are already adjusted for the toolchain above
(v143 toolset, SDK 10.0.26100, in-tree `softfloat`, dev-bundle lib names, and
output to `out\`). The exact set of changes is documented in
`MCPBRIDGE_PROGRESS.md` if you are porting to a different machine.

A note on the shell: this environment sets
`NoDefaultCurrentDirectoryInExePath=1`, which breaks the generators' post-build
steps (they run `build68k.exe` from the project dir). Clear it before building.

```powershell
Remove-Item Env:NoDefaultCurrentDirectoryInExePath -ErrorAction SilentlyContinue
$env:PATH = "C:\Program Files\NASM;" + $env:PATH
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"

# Helper generators first (Release|Win32, in this order):
'build68k_msvc','genlinetoscr_msvc','genblitter_msvc','gencpu_msvc','gencomp_msvc','prowizard' |
  ForEach-Object { & $msbuild "od-win32\$_\$_.vcxproj" /p:Configuration=Release /p:Platform=Win32 /m }

# prowizard again for x64 (produces prowizard_x64.lib):
& $msbuild "od-win32\prowizard\prowizard.vcxproj" /p:Configuration=Release /p:Platform=x64 /m

# Main emulator (Test = debuggable; FullRelease = optimized):
& $msbuild "od-win32\winuae_msvc15\winuae_msvc.vcxproj" /p:Configuration=Test /p:Platform=x64 /m
```

Output: `out\winuae64.exe` (and `winuae.exe` for Win32 builds).

> Opening `od-win32\winuae_msvc15\winuae_msvc.sln` in the VS IDE works too;
> build the generator projects first, then `winuae`.

---

## 3. Run WinUAE with the bridge

mcpbridge starts automatically and listens on **`0.0.0.0:7843`** (all
interfaces, no auth — see the security note). Just run the emulator with a
config that boots a guest:

```powershell
out\winuae64.exe -f path\to\your.uae
```

Confirm it is listening:

```powershell
Get-NetTCPConnection -LocalPort 7843 -State Listen
```

The first launch triggers a Windows Firewall prompt (the exe binds a listening
socket) — allow it for private networks.

### Security note

The listener binds `0.0.0.0` with **no authentication**. Anyone who can reach
port 7843 can move the mouse, type, reset the machine, read/poke memory, and
read files the guest can see. Fine on a trusted LAN; if you take the machine
somewhere untrusted, firewall the port at the OS level. To restrict to the
local machine, change `INADDR_ANY` to `INADDR_LOOPBACK` in `mcpbridge.cpp`.

---

## 4. Connect an MCP client

The relay (`tools/mcp-winuae-bridge.py`) shuttles the client's stdio to the
emulator's TCP socket and answers the MCP handshake itself, so the server is
always available even before the emulator starts.

### Claude Desktop

Edit `%APPDATA%\Claude\claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "winuae": {
      "command": "py",
      "args": ["-3", "C:\\Dev\\source\\WinUAE\\tools\\mcp-winuae-bridge.py"]
    }
  }
}
```

Restart Claude Desktop. The `winuae` server appears in the MCP panel with its
tools, regardless of whether the emulator is running yet.

### Claude Code

```
claude mcp add winuae -- py -3 C:\Dev\source\WinUAE\tools\mcp-winuae-bridge.py
```

### Verifying without a client (optional)

```powershell
$req = @(
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}',
  '{"jsonrpc":"2.0","method":"notifications/initialized"}',
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
) -join "`n"
$req += "`n"
$req | py -3 tools\mcp-winuae-bridge.py
```

You should get three JSON frames back (the notification gets no reply).

### Relay environment overrides

| Var | Default |
|---|---|
| `MCP_WINUAE_HOST` | `127.0.0.1` |
| `MCP_WINUAE_PORT` | `7843` |
| `MCP_WINUAE_CACHE` | `%LOCALAPPDATA%\mcp-winuae-tools.json` |

---

## 5. Start-order independence

The relay does **not** require the emulator to be up:

| State | Behavior |
|---|---|
| Emulator **up** | Tools served live; calls proxied to the emulator. |
| Emulator **down** | Server stays connected; `tools/list` is served from the on-disk cache (captured the last time the emulator was reachable); `tools/call` returns a clean *"WinUAE is not running"* result. |
| Emulator restarts | Relay reconnects automatically and emits `notifications/tools/list_changed`. |

Call **`winuae_status`** (answered by the relay, no emulator needed) to check
reachability before issuing real actions; the relay also emits
`notifications/winuae_connected` / `notifications/winuae_disconnected` on
transitions.

---

## 6. Tool catalog (46 emulator tools + `winuae_status`)

**Input**
`type_text`, `mouse_move` (absolute, closed-loop in Intuition screen pixels),
`mouse_move_rel`, `mouse_button`, `mouse_click`, `mouse_scroll`, `key_down`,
`key_up`, `key_press`, `get_pointer_pos`, `mousehack_status`,
`get_screen_geometry`. Keyboard tools are serialized and blocking — call them
back-to-back without delays.

**Machine control**
`reset` (soft/hard/keyboard), `pause`, `resume`, `toggle_pause`, `is_paused`,
`quit`, `set_speed` (turbo/max/original/balanced).

**State & media**
`save_state`, `load_state`, `disk_insert`, `disk_eject`, `disk_list`,
`mount_dir` (mount a host directory as a guest volume at runtime),
`get_config`, `set_config`.

**Inspection & debug**
`get_cpu_state`, `memory_read`, `memory_write`, `find_in_memory`, `debug`
(passthrough to the built-in debugger CLI), `disassemble`,
`breakpoint_set`/`breakpoint_clear`/`breakpoint_list`/`breakpoint_continue`
(hit parks the CPU at the exact PC; inspect with the tools above; resume to
continue).

**Capture**
`screenshot` (native Amiga resolution or host-window scale, PNG),
`audio_levels` (Paula channel snapshot), `audio_record` (mixed output → WAV).

**I/O & events**
`serial_read`, `serial_write` (guest serial port, works with no host serial
device), `inject_event` (fire any built-in `AKS_*` action / `KEY_RAW_*` /
event by name).

**Synchronization**
`wait_for_idle`, `wait_for_pixel`, `wait_for_region_change`.

**Relay-local**
`winuae_status`.

### Notifications

`notifications/breakpoint`, `notifications/screen_mode_changed`,
`notifications/paused`, `notifications/resumed`, `notifications/reset`,
`notifications/config_changed`, `notifications/mousehack_changed`
(from the emulator), plus `notifications/winuae_connected` /
`notifications/winuae_disconnected` / `notifications/tools/list_changed`
(from the relay).

---

## 7. Usage notes

- **Mouse coordinates** are Intuition screen pixels (`(0,0)` = screen
  top-left), the same space `get_pointer_pos` and the native screenshot use.
  `mouse_move` closed-loops on the live pointer position, so it is accurate in
  windowed, maximized, and fullscreen modes alike.
- **`screenshot scale:"native"`** returns the Amiga chipset buffer at native
  resolution — pixel-for-pixel aligned with `mouse_move` targets. `scale:"host"`
  returns the upscaled host-window view.
- **Absolute mouse via `mouse_move`** works in every config (closed-loop). The
  legacy `space:"amiga"` rtarea path only moves the pointer when the guest's
  magic-mouse handler is alive (`mousehack_status`).
- **`type_text`** is US-keyboard ASCII; newline = Return. Tools block until the
  guest consumes the input, so sequences are reliable without client sleeps.
- **`mount_dir`** needs the guest OS booted; the volume appears on Workbench
  within ~1s.

---

## 8. Repo layout (fork additions)

```
mcpbridge.cpp                 the bridge: listener, JSON-RPC, tools, queues
include/mcpbridge.h           init/shutdown/drain exports
include/jsmn.h                vendored single-header JSON tokenizer (MIT)
tools/mcp-winuae-bridge.py    resilient stdio<->TCP relay
tools/demo*.ps1               example MCP-client driver scripts
MCPBRIDGE_PROGRESS.md         full design doc + build/port notes
```

Upstream-side hooks live in `main.cpp` (init), `inputdevice.cpp` (drain +
pointer read), `debug.cpp` (breakpoints), `audio.cpp` / `serial_win32.cpp` /
`sounddep/sound.cpp` (taps), and `keybuf.cpp` (inject-active probe).

---

## 9. Building upstream WinUAE (original instructions)

The original upstream build notes (VS 2017, 32-bit Release generators, etc.)
are preserved for reference:

1. Requirements: Windows 7 32-bit/64-bit or newer.
2. Visual Studio 2017 Community, "Desktop Development with C++" with:
   "Support Windows XP for C++", "Windows 8.1 SDK UCRT SDK",
   "Windows 10 SDK 10.0.17763.0".
3. Install the Windows Driver Kit (WDK) 16299 (1709) or newer.
4. Download `winuaeinclibs.zip`, create `c:\dev`, extract so you have
   `c:\dev\include` and `c:\dev\lib`.
   (https://download.abime.net/winuae/files/b/winuaeinclibs.zip)
5. Download/clone the WinUAE source.
6. Download `aros.rom.cpp.zip` and extract into the source directory.
   (https://download.abime.net/winuae/files/b/aros.rom.cpp.zip)
7. Install NASM and put it in PATH (https://www.nasm.us/).
8. Open `od-win32\winuae_msvc15\winuae_msvc.sln` (ignore the "Unsupported"
   message).
9. Optionally unload projects not needed: uaeunp, consolewrapper, decompress,
   fdrawcmd, ipctester, resourcedll, singlefilehelper, wix.
10. Change to 32-bit Release mode.
11. Build in order: build68k, genlinetoscr, genblitter, gencpu, gencomp,
    prowizard, unpackers.
12. Switch to Test (debug) or FullRelease (optimized), pick 32/64-bit, compile.
