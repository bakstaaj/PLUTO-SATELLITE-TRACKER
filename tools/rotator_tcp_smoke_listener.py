#!/usr/bin/env python3
"""
Rotator TCP smoke-test listener.

Use this on the MSYS2/Windows development machine to verify the exact TCP bytes
the Pluto sends for rotator adapters before connecting actual rotator hardware.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import socket
import sys
from pathlib import Path
from typing import Optional


def utc_now() -> str:
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def escape_bytes(data: bytes) -> str:
    return data.decode("utf-8", errors="replace").replace("\r", "\\r").replace("\n", "\\n")


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def default_reply(protocol: str) -> bytes:
    p = protocol.lower().strip()
    if p == "hamlib":
        return b"RPRT 0\n"
    if p == "easycomm2":
        return b"\n"
    if p == "yaesu_gs232":
        return b"\n"
    if p == "none":
        return b""
    raise ValueError(f"unsupported protocol reply mode: {protocol}")


def write_log(log_path: Optional[Path], text: str) -> None:
    if not log_path:
        return
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("a", encoding="utf-8") as f:
        f.write(text)
        if not text.endswith("\n"):
            f.write("\n")


def handle_client(conn: socket.socket, addr, args, log_path: Optional[Path]) -> None:
    conn.settimeout(args.client_timeout)
    chunks: list[bytes] = []

    while True:
        try:
            data = conn.recv(4096)
        except socket.timeout:
            break

        if not data:
            break

        chunks.append(data)
        if b"\n" in data or b"\r" in data:
            break

    payload = b"".join(chunks)

    lines = [
        "",
        f"[{utc_now()}] CONNECTION from {addr[0]}:{addr[1]}",
        f"BYTES: {len(payload)}",
        f"TEXT:  {escape_bytes(payload)}",
        f"HEX:   {hex_bytes(payload)}",
    ]

    reply = args.reply.encode("utf-8") if args.reply is not None else default_reply(args.protocol)
    if reply:
        try:
            conn.sendall(reply)
            lines.append(f"REPLY_TEXT: {escape_bytes(reply)}")
            lines.append(f"REPLY_HEX:  {hex_bytes(reply)}")
        except OSError as exc:
            lines.append(f"REPLY_ERROR: {exc}")

    output = "\n".join(lines)
    print(output, flush=True)
    write_log(log_path, output)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Listen for Pluto rotator TCP commands and print received bytes.")
    parser.add_argument("--host", default="0.0.0.0", help="Listener bind address. Use 0.0.0.0 to listen on all local interfaces.")
    parser.add_argument("--port", type=int, default=4533, help="Listener TCP port.")
    parser.add_argument("--protocol", choices=["hamlib", "easycomm2", "yaesu_gs232", "none"], default="hamlib", help="Reply mode.")
    parser.add_argument("--reply", default=None, help="Override reply text. Use '' for no reply.")
    parser.add_argument("--max-connections", type=int, default=20, help="Exit after this many client connections.")
    parser.add_argument("--client-timeout", type=float, default=1.5, help="Seconds to wait for more bytes from one client.")
    parser.add_argument("--log", default="", help="Optional log file path.")
    args = parser.parse_args(argv)

    if not (1 <= args.port <= 65535):
        parser.error("--port must be between 1 and 65535")
    if args.max_connections < 1:
        parser.error("--max-connections must be at least 1")

    log_path = Path(args.log).expanduser() if args.log else None

    banner = "\n".join([
        f"[{utc_now()}] Rotator TCP smoke-test listener starting",
        f"BIND:     {args.host}:{args.port}",
        f"PROTOCOL: {args.protocol}",
        "NOTE: In the Pluto UI, set Host to this computer's LAN IP address, not 0.0.0.0.",
        "      Press Ctrl+C to stop.",
        "",
    ])
    print(banner, flush=True)
    write_log(log_path, banner)

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((args.host, args.port))
            server.listen(5)

            count = 0
            while count < args.max_connections:
                conn, addr = server.accept()
                count += 1
                with conn:
                    handle_client(conn, addr, args, log_path)

    except KeyboardInterrupt:
        print("\nStopped by user.", flush=True)
        return 0
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"\n[{utc_now()}] Max connections reached; exiting.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
