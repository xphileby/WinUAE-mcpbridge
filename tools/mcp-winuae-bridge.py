#!/usr/bin/env python3
"""
Resilient MCP stdio<->TCP relay for the winuae mcpbridge listener.

Spawned by an MCP client (Claude Code, Claude Desktop, IDE plugins) as a
stdio subprocess. Unlike a dumb pipe, this relay does NOT require WinUAE to be
running when the client starts it. It answers the MCP handshake itself, serves
a cached tool list, and proxies tool calls to WinUAE only when the emulator is
actually up. So:

  * Start the client and WinUAE in ANY order.
  * Restart WinUAE freely mid-session; the MCP server stays "connected".
  * tools/call while WinUAE is down returns a clean error (not a dead server).

How it works:
  - initialize / ping / notifications/initialized: answered locally.
  - tools/list: served from an on-disk cache (~/.cache/mcp-winuae-tools.json)
    captured the last time WinUAE was reachable. Cold start with no cache and
    no WinUAE returns an empty list; once WinUAE connects, the relay refreshes
    the cache and emits notifications/tools/list_changed.
  - tools/call (and any other method): forwarded to WinUAE if connected;
    otherwise an error result is returned immediately.
  - A background thread keeps (re)connecting to WinUAE, forwards the
    emulator's responses and spontaneous notifications to stdout, and
    refreshes the tool cache on each (re)connect.

Env overrides:
  MCP_WINUAE_HOST   default 127.0.0.1
  MCP_WINUAE_PORT   default 7843
  MCP_WINUAE_CACHE  default <user cache dir>/mcp-winuae-tools.json
"""

import json
import os
import socket
import sys
import threading
import time

HOST = os.environ.get("MCP_WINUAE_HOST", "127.0.0.1")
PORT = int(os.environ.get("MCP_WINUAE_PORT", "7843"))


def _default_cache_path():
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_CACHE_HOME") \
        or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "mcp-winuae-tools.json")


CACHE_PATH = os.environ.get("MCP_WINUAE_CACHE", _default_cache_path())

# Internal request id used to fetch tools/list from WinUAE for caching. The
# forwarder filters responses bearing this id so they never reach the client.
PROBE_ID = "__relay_tools_probe__"

SERVER_INFO = {"name": "winuae-mcpbridge-relay", "version": "1.1"}
PROTOCOL_VERSION = "2024-11-05"

# Relay-local tool: lets the client check whether the emulator is reachable
# WITHOUT touching WinUAE, so it can avoid firing real actions when the
# emulator is down. Always present in tools/list, answered by the relay.
RELAY_STATUS_TOOL = {
    "name": "winuae_status",
    "description": (
        "Report whether WinUAE (the emulator) is currently connected to this "
        "MCP relay. Answered by the relay itself, so it works even when the "
        "emulator is down. Returns {connected, host, port, "
        "seconds_since_change, tools_cached}. Call this before issuing real "
        "tool actions if you are unsure the emulator is running."
    ),
    "inputSchema": {"type": "object", "properties": {}},
}


def out(obj):
    """Write one JSON-RPC frame to stdout (the MCP client)."""
    data = (json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8")
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def log(msg):
    sys.stderr.write("mcp-winuae-bridge: " + msg + "\n")
    sys.stderr.flush()


class Relay:
    def __init__(self):
        self.lock = threading.Lock()
        self.sock = None              # connected TCP socket, or None
        self.tools = self._load_cache()
        self.tools_sig = json.dumps(self.tools, sort_keys=True)
        self.client_ready = False     # client has sent initialize
        self.stop = False
        self.connected = False        # WinUAE reachable (debounced view)
        self.changed_at = time.time() # last connect/disconnect transition

    def _set_connected(self, value):
        with self.lock:
            if self.connected == value:
                return
            self.connected = value
            self.changed_at = time.time()
        if self.client_ready:
            out({"jsonrpc": "2.0",
                 "method": "notifications/winuae_connected" if value
                 else "notifications/winuae_disconnected",
                 "params": {"connected": value}})

    # ---- tool cache -----------------------------------------------------
    def _load_cache(self):
        try:
            with open(CACHE_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, list):
                return data
        except (OSError, ValueError):
            pass
        return []

    def _save_cache(self, tools):
        try:
            os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
            with open(CACHE_PATH, "w", encoding="utf-8") as f:
                json.dump(tools, f)
        except OSError:
            pass

    def _update_tools(self, tools):
        sig = json.dumps(tools, sort_keys=True)
        with self.lock:
            changed = sig != self.tools_sig
            self.tools = tools
            self.tools_sig = sig
        self._save_cache(tools)
        if changed and self.client_ready:
            out({"jsonrpc": "2.0", "method": "notifications/tools/list_changed",
                 "params": {}})

    # ---- TCP side -------------------------------------------------------
    def connector_loop(self):
        """Maintain a connection to WinUAE; forward its frames to stdout."""
        while not self.stop:
            try:
                s = socket.create_connection((HOST, PORT), timeout=2.0)
            except OSError:
                time.sleep(1.0)
                continue
            s.settimeout(None)
            with self.lock:
                self.sock = s
            self._set_connected(True)
            log(f"connected to WinUAE at {HOST}:{PORT}")
            # Refresh the tool cache from the live server.
            try:
                s.sendall((json.dumps(
                    {"jsonrpc": "2.0", "id": PROBE_ID, "method": "tools/list"}
                ) + "\n").encode("utf-8"))
            except OSError:
                pass
            self._read_forward(s)
            with self.lock:
                self.sock = None
            self._set_connected(False)
            try:
                s.close()
            except OSError:
                pass
            log("WinUAE connection closed; will retry")
            time.sleep(0.5)

    def _read_forward(self, s):
        """Read NDJSON from WinUAE; forward to stdout except the tool probe."""
        buf = b""
        while not self.stop:
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                # Intercept the probe response to refresh the tool cache.
                try:
                    obj = json.loads(line)
                except ValueError:
                    obj = None
                if isinstance(obj, dict) and obj.get("id") == PROBE_ID:
                    tools = (obj.get("result") or {}).get("tools")
                    if isinstance(tools, list):
                        self._update_tools(tools)
                    continue
                sys.stdout.buffer.write(line + b"\n")
                sys.stdout.buffer.flush()

    def _send_to_winuae(self, raw_line_bytes):
        with self.lock:
            s = self.sock
        if s is None:
            return False
        try:
            s.sendall(raw_line_bytes)
            return True
        except OSError:
            return False

    # ---- MCP request dispatch (stdin side) ------------------------------
    def handle(self, raw_line):
        try:
            req = json.loads(raw_line)
        except ValueError:
            return  # ignore malformed input
        if not isinstance(req, dict):
            return
        method = req.get("method")
        rid = req.get("id")
        is_request = "id" in req

        if method == "initialize":
            self.client_ready = True
            if is_request:
                out({"jsonrpc": "2.0", "id": rid, "result": {
                    "protocolVersion": PROTOCOL_VERSION,
                    "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": SERVER_INFO}})
            return
        if method == "notifications/initialized":
            return
        if method == "ping":
            if is_request:
                out({"jsonrpc": "2.0", "id": rid, "result": {}})
            return
        if method == "tools/list":
            with self.lock:
                tools = list(self.tools)
            # Always advertise the relay-local status tool.
            tools = [RELAY_STATUS_TOOL] + tools
            if is_request:
                out({"jsonrpc": "2.0", "id": rid, "result": {"tools": tools}})
            return

        # Relay-local tool: winuae_status (answered without touching WinUAE).
        if method == "tools/call" and \
                (req.get("params") or {}).get("name") == "winuae_status":
            with self.lock:
                connected = self.connected
                since = round(time.time() - self.changed_at, 1)
                ncached = len(self.tools)
            payload = {"connected": connected, "host": HOST, "port": PORT,
                       "seconds_since_change": since, "tools_cached": ncached}
            if is_request:
                out({"jsonrpc": "2.0", "id": rid, "result": {
                    "content": [{"type": "text",
                                 "text": json.dumps(payload)}]}})
            return

        # Everything else (tools/call, etc.) -> proxy to WinUAE.
        if self._send_to_winuae((raw_line.rstrip("\n") + "\n").encode("utf-8")):
            return  # response will arrive via the forwarder
        # WinUAE not connected.
        if is_request:
            out({"jsonrpc": "2.0", "id": rid, "result": {
                "isError": True,
                "content": [{"type": "text", "text":
                             "WinUAE is not running (no mcpbridge connection on "
                             f"{HOST}:{PORT}). Start the emulator and try again."}]}})

    def run(self):
        t = threading.Thread(target=self.connector_loop, daemon=True)
        t.start()
        buf = b""
        try:
            while True:
                chunk = sys.stdin.buffer.read1(65536)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if line.strip():
                        self.handle(line.decode("utf-8", "replace"))
        except (OSError, KeyboardInterrupt):
            pass
        self.stop = True


if __name__ == "__main__":
    Relay().run()
