#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Record a Source 2 game's developer console (VConsole2 protocol).

Games started with -vconport <port> listen on that TCP port; SteamVR Home is
started with -vconport 29009 (tools.vrmanifest). This connects, retrying until
the game is up and reconnecting if it restarts, and writes the console's
printed lines with wall-clock timestamps.

    vconsole_capture.py [--port 29009] [--host 127.0.0.1] [--out FILE] [--seconds N]

Messages are framed as: 4-byte type, uint32 version, uint16 total length
(header included), uint16 handle, all big-endian. PRNT messages carry a
channel id and fixed fields, then the NUL-terminated text.
"""
import argparse
import datetime
import socket
import struct
import sys
import time

HEADER = struct.Struct(">4sIHH")
PRNT_TEXT_OFFSET = 28  # channel id and fixed fields before the text


def text_of(body):
    text = body[PRNT_TEXT_OFFSET:].split(b"\0", 1)[0]
    return text.decode("utf-8", "replace").rstrip("\r\n")


def capture(host, port, out, deadline):
    while deadline is None or time.time() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=2)
        except OSError:
            time.sleep(1)
            continue
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        out.write(f"{stamp} [capture] connected to {host}:{port}\n")
        out.flush()
        sock.settimeout(1)
        buffer = b""
        while deadline is None or time.time() < deadline:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                break
            buffer += chunk
            while len(buffer) >= HEADER.size:
                kind, _, length, _ = HEADER.unpack_from(buffer)
                if length < HEADER.size:  # out of step: drop a byte and resync
                    buffer = buffer[1:]
                    continue
                if len(buffer) < length:
                    break
                body, buffer = buffer[HEADER.size:length], buffer[length:]
                if kind == b"PRNT":
                    stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    for line in text_of(body).splitlines() or [""]:
                        out.write(f"{stamp} {line}\n")
                    out.flush()
        sock.close()
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        out.write(f"{stamp} [capture] disconnected\n")
        out.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=29009)
    parser.add_argument("--out", help="file to append to (default: standard output)")
    parser.add_argument("--seconds", type=float, help="stop after this long")
    args = parser.parse_args()
    deadline = time.time() + args.seconds if args.seconds else None
    out = open(args.out, "a", buffering=1) if args.out else sys.stdout
    try:
        capture(args.host, args.port, out, deadline)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
