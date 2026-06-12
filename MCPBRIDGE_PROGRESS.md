# MCP bridge for WinUAE — progress + next steps

Working notes for the in-tree MCP server that exposes WinUAE input / control
to an MCP client over TCP. Captured at a beta-build checkpoint before
rebasing onto tag `6030`.

## Goal

Embed a small JSON-RPC 2.0 server inside `winuae.exe` that an MCP client
(via a tiny Python stdio↔TCP relay) can drive. Surface:

- Inputs: keyboard, mouse, scancodes, modifiers, text typing
- Control: reset, pause/resume, save/load state
- Disk: insert/eject/list
- Config: runtime set_config
- Read-only: mousehack_status, get_screen_geometry, is_paused, disk_list

## Architecture (settled)

- One **listener thread** spawned by `mcpbridge_init(7843)` at WinUAE startup.
- Binds `0.0.0.0:7843` (single-client; second connection gets a JSON-RPC error
  and is closed). No auth token — trust the LAN.
- Reads NDJSON, dispatches per JSON-RPC method.
- **Command queue** (`std::mutex` + `std::deque<mcp_cmd>`) for anything that
  must run on the emulation thread.
- **`mcpbridge_drain()`** called from the top of `inputdevice_read()` so the
  queue drains at vblank (~50 Hz). Uses `try_lock` so the emu thread never
  stalls.
- A few calls (`pause`, `resume`, `toggle_pause`, `reset`, all read-only
  queries) **bypass the queue** and run on the receiver thread, because the
  drain doesn't fire while paused — a queued `resume` would deadlock.

## Files touched / added

### New files
| Path | Purpose |
|---|---|
| `mcpbridge.cpp` | Listener, JSON-RPC dispatcher, tool implementations (~700 LOC) |
| `include/mcpbridge.h` | 3 exports: `mcpbridge_init/shutdown/drain` |
| `include/jsmn.h` | jsmn 1.1 (MIT), single-header JSON tokenizer (471 LOC, fetched verbatim from https://raw.githubusercontent.com/zserge/jsmn/master/jsmn.h) |

### Edits to existing files
| File | Edit |
|---|---|
| `include/inputdevice.h` | After `setmousestate(...)` line: add `extern void inputdevice_mh_abs(int x, int y, uae_u32 buttonbits);` |
| `inputdevice.cpp` | (1) Add `#include "mcpbridge.h"` next to `#include "keybuf.h"`. (2) Remove `static` from `inputdevice_mh_abs(int x, int y, uae_u32 buttonbits)`. (3) Insert `mcpbridge_drain();` as the first line of `inputdevice_read(void)`. |
| `main.cpp` | (1) Add `#include "mcpbridge.h"` after `#include "SDL.h"` block. (2) Add `mcpbridge_init(7843);` immediately after `keybuf_init();` in `real_main2`. |
| `od-win32/winuae_msvc15/winuae_msvc.vcxproj` | Add `<ClCompile Include="..\..\mcpbridge.cpp" />` in the ItemGroup next to `main.cpp`. |

## Tag 6030 additional fixes (on top of the beta fixes below)

After checkout to tag `6030` (commit `857f48fc`), the same environment fixes
below are needed, PLUS these 6030-specific vcxproj edits in
`od-win32/winuae_msvc15/winuae_msvc.vcxproj`:

| Replace | With | Why |
|---|---|---|
| `zlibstat.lib` | `zs.lib` | The dev libs bundle ships `zs.lib`, not `zlibstat.lib` |
| `libFLAC_static.lib` | `FLAC.lib` | Same — only `FLAC.lib` shipped |
| `enet_x64.lib` (x64 only) | `enet.lib` | `dev/lib/x64/enet.lib` exists; no `enet_x64.lib` shipped |
| `;softfloat.lib` (and `softfloat.lib;`) | (remove) | `softfloat.lib` isn't shipped. The vcxproj already has `softfloat/softfloat*.cpp` in `ClCompile` entries near line 1160 — those compile in-tree, so the standalone lib isn't needed |
| `prowizard_x64.lib` ↔ `prowizard.lib` per arch | Restore arch-correct per line | Easy to mangle with a global Replace. Win32 has `enet.lib;lzmalib.lib;prowizard.lib;`; x64 has `enet.lib;prowizard_x64.lib;lzmalib.lib;` — different surrounding lib order, use that to target the substitution per line. |

`prowizard.vcxproj` on 6030 has a per-arch OutDir/TargetName pattern:
- Win32 → `od-win32/lib/prowizard.lib`
- x64   → `od-win32/lib/prowizard_x64.lib` (note `_x64` suffix from `TargetName`)

So when building on 6030 the helpers step needs to build prowizard TWICE — once
for Win32 (`Release|Win32`) and once for x64 (`Release|x64`) — to produce both
`.lib`s. `od-win32/lib/` is the helper-output dir (auto-created, not under git).

The dev junctions (`C:\Dev\include` → repo `dev/include`, `C:\Dev\lib` → repo
`dev/lib`) carry across the tag checkout without changes.

Working tree is currently on detached HEAD at tag 6030. For ongoing work
create a branch: `git switch -c mcpbridge-6030`.

## Environment fixes that were needed to build the beta (may or may not
apply to tag 6030 — re-verify after checkout)

- **NASM** installed via `winget install NASM.NASM` to `C:\Program Files\NASM\`. Not in PATH; prepend it for each MSBuild call.
- **Dev libs/includes** are at `C:\Dev\source\WinUAE\dev\` but project hardcodes `C:\dev\include` / `C:\dev\lib\{x86|x64|arm64}`. Resolved with junctions:
  - `C:\Dev\include` → `C:\Dev\source\WinUAE\dev\include`
  - `C:\Dev\lib` → `C:\Dev\source\WinUAE\dev\lib`
  - (case-insensitive resolution handles `C:\dev\...` references)
- **All 11 vcxproj files** had `PlatformToolset=v145` → changed to `v143` (only v143 is installed; v145 is VS 2026).
- **`winuae_msvc.vcxproj` only**: `10.0.18362.0\km` → `10.0.26100.0\km` (only 26100 has the WDK km headers on this machine); `OutDir d:\amiga\` → `C:\Dev\source\WinUAE\out\` (D: is a CD-ROM here).
- **`winuae_msvc.vcxproj` only**: `<LanguageStandard_C>stdclatest</LanguageStandard_C>` × 10 → `stdc17` (MSBuild 17.6 doesn't know `stdclatest`).
- **Build runtime quirk**: `NoDefaultCurrentDirectoryInExePath=1` is set in this process; cmd.exe inside post-build steps then can't find `build68k.exe`. Workaround: `Remove-Item Env:NoDefaultCurrentDirectoryInExePath` before invoking MSBuild.

## Header order gotcha

`<winsock2.h>` and `<ws2tcpip.h>` MUST come before `sysdeps.h`, because
`sysdeps.h` pulls in `<winsock.h>` v1 which conflicts. Pattern (copied from
`od-win32/bsdsock.cpp`):

```cpp
#include <winsock2.h>
#include <ws2tcpip.h>

#include "sysconfig.h"
#include "sysdeps.h"
// ... rest of includes
```

## Tool surface — what's implemented (22 tools)

**Inputs (10):**
- `type_text(text)`
- `mouse_move(x, y, *space)` — default `space:"amiga"` calls `inputdevice_mh_abs(x,y,0)`; `space:"host"` calls `setmousestate(0,0,x,1); setmousestate(0,1,y,1)`. **Amiga path requires mousehack_alive=1 to be visible.**
- `mouse_button(button, state)` — `setmousebuttonstate(0, button, state)`
- `mouse_click(*button=0, *count=1)` — N press/release pairs
- `mouse_scroll(*dx, *dy)` — axes 3 (h) and 2 (v) via setmousestate
- `key_down(key)` / `key_up(key)` — `record_key_direct((scancode<<1) | (up?1:0), true)`
- `key_press(key, *modifiers[])` — press mods, press key, release key, release mods
- `mousehack_status()` — reads `currprefs.input_tablet`, `mousehack_alive()`, `input_mouse_untrap`
- `get_screen_geometry()` — reads `adisplays[0].gfxvidinfo.outbuffer`, `picasso96_state`

**Control / state (12):**
- `reset(*kind)` — `kind`: `"soft"` (default) / `"hard"` / `"keyboard"` → `uae_reset(hard, kbd)` immediate
- `pause` / `resume` / `toggle_pause` — `pausemode(1|0|-1)` immediate
- `is_paused` — reads `pause_emulation`
- `save_state(path)` — `save_state(path, STATE_SAVE_DESCRIPTION)` queued
- `load_state(path)` — `restore_state(path)` queued
- `disk_insert(drive, path)` — queued `disk_insert(drive, w(path))`
- `disk_eject(drive)` — queued `disk_eject(drive)`
- `disk_list()` — reads `currprefs.floppyslots[0..3]`
- `set_config({line} | {key,value})` — `cfgfile_parse_line(&changed_prefs, ...)` then ` ... (&currprefs, ...)`, then `set_config_changed()`, then `inputdevice_updateconfig(&changed_prefs, &currprefs)`

**Key name table:** case-insensitive, covers a–z, 0–9, all symbols on the US keyboard, Space/Tab/Enter/Backspace/Escape/Delete/Help, arrows, F1–F10, all modifiers (LShift/RShift, Ctrl, LAlt/RAlt, LAmiga/RAmiga), with aliases (Return=Enter, Caps=CapsLock, LWin=LAmiga, etc.). Also accepts `0xNN` raw Amiga scancode.

## Verified live in step 1–3

- TCP listener on `0.0.0.0:7843` (netstat-confirmed)
- Single-client rejection sends `{"error":"single-client server; already connected"}`
- `initialize` / `ping` / `tools/list` / `tools/call` round-trip cleanly
- `key_press(e, modifiers:[RAmiga])` opened Workbench's "Execute a file" dialog
- `type_text("echo HELLO_FROM_MCP")` rendered in the dialog's Command field
- `pause`→`is_paused`(true)→`resume`→`is_paused`(false) full cycle
- `reset kind=hard` → black → ~8s → Workbench back
- `save_state` produced a 280 MB `.uss` file on disk
- `mouse_move space:"host"` visibly moved pointer to mid-screen
- `set_config magic_mouse=false` flipped the field through `cfgfile_parse_line`
- `disk_list` returned `{drives:[{drive:0,path:"",type:-1},...]}`

## Mouse control findings (verified by driving Workbench mouse-only)

Tested end-to-end by opening Workbench disk → Prefs drawer → Input editor
using only `mouse_move` + `mouse_button` (no keyboard). See
`tools/demo-mouse-nav.ps1` for the working navigation pattern.

**The semantics of `mouse_move(space:'host')` when mousehack is NOT alive
are RELATIVE, not absolute.** In `setmousestate`'s isabs branch, `*oldm_p`
is reset to 0 at the end of every call (`inputdevice.cpp` ~line 9868), so
each call computes `delta = data - 0 = data`. The "absolute coordinate" you
pass is actually applied as a relative delta, metered into the guest's
hardware mouse counters over the following frames.

Practical rules for reliable pointer control (AmigaOS 3.x, no mousehack):

1. **Treat each `mouse_move(space:'host')` call as `move_rel(dx, dy)`.**
2. **Keep each delta ≤ 50 per axis.** The guest mouse driver reads the
   8-bit hardware counters (`MOUSE0DAT`) once per vsync and interprets the
   difference as signed 8-bit; accumulated deltas > 127 in one frame alias
   (e.g. +200 reads as −56). Two queued 50-unit steps landing in the same
   vsync still sum to a safe 100.
3. **Pace calls ≥ 90 ms apart.** The bridge replies when the command is
   *queued*, not drained, so back-to-back calls can land in one vsync.
4. **Calibrate by pinning**: ~32 paced steps of (−50,−50) pins the pointer
   at guest (0,0) regardless of starting position. From there, relative
   moves are exact (scale is 1:1 with native-screenshot pixels).
5. **Origin offset**: in `screenshot(scale:'native')` images, guest (0,0)
   renders at pixel (54,28) — the overscan border. So to click on
   screenshot pixel (px,py): pin, then move_rel(px−54, py−28).
6. **Clicks**: use `mouse_button` down/up pairs with ~90 ms between down
   and up. A queued `mouse_click count:2` drains in one vsync and the guest
   may not register it. For a double-click: click, ~60 ms gap, click.

Future bridge improvement: add a real `mouse_move_rel(dx,dy)` tool that
chunks + paces inside the drain (one chunk per vsync), and make
`space:'host'` truly absolute by doing pin+walk internally.

## Open issues — re-investigate after rebase to 6030

1. **`cfgfile_parse_line(absolute_mouse=N)` silently no-ops on this build.**
   Other keys parse fine (e.g. `magic_mouse`). The `cfgfile_strval` chain at
   `cfgfile.cpp:3827` looks correct on inspection. Worked around with a
   direct-write shortcut for `absolute_mouse=` in `CMD_SET_CONFIG`. Find the
   root cause; might affect other `cfgfile_strval`-driven options too.

2. **`load_state` leaves the chipset display in a wedged-looking intermediate.**
   File round-trips, but after the restore the screen freezes on whatever
   frame is mid-render and doesn't resume cleanly. Possible missing follow-up
   like `savestate_restore_finish()` or running the restore from a different
   point in the main loop. The `.uss` file itself is fine; verified by
   ability to do `reset hard` afterwards.

3. **Mousehack-alive dependency for Amiga-coord mouse.** Currently
   `input_tablet=0`, `mousehack_alive=0` in this OS 3.2 config — the magic-
   mouse Amiga-side handler isn't pinged. With `set_config absolute_mouse=
   tablet` the input_tablet flips to 2 but mousehack still doesn't go alive.
   Investigate whether the bootrom rtarea hook needs additional bring-up, or
   whether 3.2 needs a specific commodity/daemon enabled to make magic mouse
   call into rtarea.

## Smoke-test recipe

```powershell
# Launch
Remove-Item Env:NoDefaultCurrentDirectoryInExePath -ErrorAction SilentlyContinue
C:\Dev\source\WinUAE\out\winuae64.exe -f C:\Dev\source\WinUAE\vm\mcamiga-aga-local.uae

# Wait for port
do { Start-Sleep 1 } until ((Get-NetTCPConnection -LocalPort 7843 -State Listen -EA SilentlyContinue))

# One-liner client
function Send-Mcp([string]$json) {
  $c = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 7843)
  $s = $c.GetStream()
  $w = [System.IO.StreamWriter]::new($s); $w.NewLine="`n"; $w.AutoFlush=$true
  $r = [System.IO.StreamReader]::new($s)
  $w.WriteLine($json); $line = $r.ReadLine(); $c.Close(); return $line
}

Send-Mcp '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
Send-Mcp '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
Send-Mcp '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"type_text","arguments":{"text":"echo hi\n"}}}'
```

For visible feedback you need a focused input target in the guest. Easiest:
`key_press` with `key:"e", modifiers:["RAmiga"]` to open the Execute Command
requester, then `type_text`.

## Next planned steps

### Step 4 — Tier-2 inspection / debugging tools (~1–2 days)
- `memory_read(addr, length, *space)` — `get_byte/word/long` per byte; `space` = `"chip"|"fast"|"virtual"|"physical"`
- `memory_write(addr, bytes_b64)` — symmetric
- `debug(command)` — passthrough to `debug_parser()` (already used by uaeipc). Single tool unlocks disassembler, breakpoints, memory dump, search
- `get_cpu_state()` — structured `{d0..d7, a0..a7, pc, sr, model, cycles}` from `regs`
- `find_in_memory(pattern_b64, *mask, addr_range)` — scan helper
- `disassemble(addr, count)` — structured wrap of debug's `d` command

### Step 5 — Screenshot (was originally separate; pull in early because we
need it for testing)
- `screenshot(*scale, *format)` — `scale:"native"` returns the chipset-line
  buffer at native Amiga resolution (1:1 with `mouse_move(amiga)` coords);
  `scale:"window"` returns the upscaled host view. Use `libpng16` (already
  linked). Response includes coordinate transform metadata.

### Step 6 — Synchronization helpers
- `wait_for_idle(timeout_ms, *quiet_ms)` — poll CPU activity counter
- `wait_for_pixel(x, y, rgb, *tolerance, timeout_ms)` — poll pixel against the chipset buffer
- `wait_for_region_change(x, y, w, h, timeout_ms)` — region hash watcher

### Step 7 — Notifications path
- Reverse send queue (`std::deque<std::string>`) + writer side of the socket
- Emission hooks for: mousehack_alive transition, screen mode change, config reload, pause/resume
- Decision made earlier (small notification set, not full event stream)

### Step 8 — Python relay + Claude Desktop config — DONE

Relay at `tools/mcp-winuae-bridge.py`. Byte-transparent: doesn't parse
JSON, doesn't re-frame, just shuttles between stdin/stdout and
`127.0.0.1:7843`. Python 3.8+. Half-closes the TCP write side on stdin EOF
so in-flight responses still arrive before exit.

Configurable via env: `MCP_WINUAE_HOST` (default `127.0.0.1`),
`MCP_WINUAE_PORT` (default `7843`).

**Claude Desktop config** (`%APPDATA%\Claude\claude_desktop_config.json` on
Windows). The `py` Windows launcher is the safest invocation since it
doesn't depend on whichever Python ends up in PATH:

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

Alternatively, if your `python` is the one you want:

```json
{
  "mcpServers": {
    "winuae": {
      "command": "python",
      "args": ["C:\\Dev\\source\\WinUAE\\tools\\mcp-winuae-bridge.py"]
    }
  }
}
```

**Restart Claude Desktop after editing.** It re-spawns MCP servers only on
launch. The mcpbridge listener inside `winuae64.exe` must already be up
before Claude tries to connect — start the emulator first.

**Smoke-test the relay outside Claude** (PowerShell):

```powershell
$req = @(
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}',
  '{"jsonrpc":"2.0","method":"notifications/initialized"}',
  '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_cpu_state"}}'
) -join "`n"
$req += "`n"
$req | py -3 "C:\Dev\source\WinUAE\tools\mcp-winuae-bridge.py"
```

Three JSON-RPC frames should come back on stdout (the notification gets no
reply, per JSON-RPC spec).

**If the relay errors out** (connection refused) the message lands on
stderr; Claude Desktop surfaces MCP-server stderr in its log panel.

### Step 9 — Tier-2 misc (lower priority, on demand)
`breakpoint_set/clear`, `set_speed`, `mount_dir`, `inject_event`,
`serial_read/write`, `audio_levels/record`.

## Open design decisions made (and why)

| # | Decision | Rationale |
|---|---|---|
| 1 | `mouse_move` default = Amiga chip coords | What an AI naturally thinks in. `space:"host"` opt-out for cases where mousehack isn't alive. |
| 2 | Bind `0.0.0.0`, no auth token | Personal dev tool on a trusted LAN. Firewall at OS level if needed. |
| 3 | Single client; second `accept` returns JSON-RPC error | Simplest reply / notification routing. |
| 4 | Small notification set in v1 | Just enough so AI doesn't have to poll for mousehack/screen/config/pause transitions. |
| 5 | TCP + Python relay (not embedded HTTP) | ~300 LOC of C++ vs ~9000 LOC mongoose. MCP framing stays in Python where it's trivial to iterate. |
