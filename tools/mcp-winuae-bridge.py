#!/usr/bin/env python3
"""
MCP stdio<->TCP relay for the winuae mcpbridge listener.

Spawned by an MCP client (Claude Desktop, IDE plugins, ...) as a subprocess.
- Reads JSON-RPC frames from stdin and forwards them to TCP 127.0.0.1:7843
- Reads responses/notifications from TCP and writes them to stdout

The relay is byte-transparent: it never parses JSON, never re-wraps frames,
and never adds or strips newlines. WinUAE's mcpbridge speaks NDJSON, MCP
speaks NDJSON, so the two endpoints are already compatible.

Configurable via env vars (override the defaults if needed):
  MCP_WINUAE_HOST   default 127.0.0.1
  MCP_WINUAE_PORT   default 7843
"""

import os
import socket
import sys
import threading

HOST = os.environ.get("MCP_WINUAE_HOST", "127.0.0.1")
PORT = int(os.environ.get("MCP_WINUAE_PORT", "7843"))


def pump_to_stdout(sock: socket.socket) -> None:
    """TCP -> stdout, until the peer closes."""
    try:
        while True:
            data = sock.recv(65536)
            if not data:
                break
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
    except OSError:
        pass


def main() -> int:
    try:
        sock = socket.create_connection((HOST, PORT), timeout=2.0)
    except OSError as e:
        sys.stderr.write(
            f"mcp-winuae-bridge: cannot connect to {HOST}:{PORT}: {e}\n"
            "Is winuae64.exe running with the mcpbridge listening?\n"
        )
        return 1

    sock.settimeout(None)

    # TCP -> stdout, in a background thread.
    pump = threading.Thread(target=pump_to_stdout, args=(sock,))
    pump.daemon = True
    pump.start()

    # stdin -> TCP, on the main thread.
    try:
        while True:
            chunk = sys.stdin.buffer.read1(65536)
            if not chunk:
                break
            sock.sendall(chunk)
    except OSError:
        pass

    # Half-close write side so the server sees end-of-input but keeps
    # delivering responses. Then give the pump a moment to flush any
    # in-flight responses before we exit.
    try:
        sock.shutdown(socket.SHUT_WR)
    except OSError:
        pass
    pump.join(timeout=1.0)
    try:
        sock.close()
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
