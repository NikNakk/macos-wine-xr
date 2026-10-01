#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Check that a transitional Monado client preserves command IDs against a newer service."""

from __future__ import annotations

import argparse
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
    args = ap.parse_args()

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


if __name__ == "__main__":
    main()
