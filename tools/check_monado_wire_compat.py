#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check that a transitional Monado client preserves command IDs against a newer service."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


COMMAND_RE = re.compile(r"^\s*(IPC_[A-Z0-9_]+),\s*$", re.MULTILINE)


def commands(path: Path) -> list[str]:
    values = COMMAND_RE.findall(path.read_text())
    if not values:
        raise RuntimeError(f"no IPC command enum entries found in {path}")
    return values


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--client-header", required=True, type=Path)
    ap.add_argument("--service-header", required=True, type=Path)
    ap.add_argument("--client-source", type=Path)
    ap.add_argument("--service-source", type=Path)
    args = ap.parse_args()

    if (args.client_source is None) != (args.service_source is None):
        raise RuntimeError("--client-source and --service-source must be supplied together")

    client = commands(args.client_header)
    service = commands(args.service_header)

    if len(service) < len(client):
        raise RuntimeError(
            f"service has fewer IPC commands ({len(service)}) than transitional client ({len(client)})"
        )

    for i, (old, new) in enumerate(zip(client, service), start=1):
        if old != new:
            raise RuntimeError(
                f"IPC command ID mismatch at {i}: transitional client={old}, clean service={new}"
            )

    extra = service[len(client):]
    print(
        f"IPC command prefix compatible: {len(client)} transitional commands preserved; "
        f"{len(extra)} clean-service command(s) appended"
    )
    if extra:
        print("appended service commands: " + ", ".join(extra))


    if args.client_source is not None:
        client_proto = args.client_source / "src/xrt/ipc/shared/proto"
        service_proto = args.service_source / "src/xrt/ipc/shared/proto"
        client_files = sorted(
            path for path in client_proto.glob("*.json") if not path.name.endswith("schema.json")
        )
        for client_path in client_files:
            service_path = service_proto / client_path.name
            if not service_path.is_file():
                raise RuntimeError(f"clean service removed legacy protocol schema {client_path.name}")
            client_schema = json.loads(client_path.read_text())
            service_schema = json.loads(service_path.read_text())
            if client_schema != service_schema:
                raise RuntimeError(
                    f"legacy protocol schema changed in {client_path.name}; "
                    "the transitional Win64 client is no longer wire-layout compatible"
                )
        print(
            f"IPC schema compatible: {len(client_files)} legacy protocol file(s) unchanged; "
            "clean service may only append new protocol files"
        )


if __name__ == "__main__":
    main()
