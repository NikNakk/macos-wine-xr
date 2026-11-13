#!/usr/bin/env python3
"""Summarize bounded DXMT scene diagnostics; never select or modify rendering."""
import argparse
import collections
import json
import re
from pathlib import Path


def summarize(lines):
    nodes, metadata, profiles = {}, {}, collections.Counter()
    upscales, truncated, attachments = [], False, 0
    for line in lines:
        match = re.search(r"SCENE (\w+) (.*)", line)
        if not match:
            continue
        kind, tail = match.groups()
        fields = dict(re.findall(r"(\w+)=([^\s]+)", tail))
        if kind == "truncated":
            truncated = True
            continue
        if "encoder" not in fields:
            continue
        encoder = int(fields["encoder"])
        node = nodes.setdefault(encoder, {"reads": set(), "writes": set()})
        if kind == "access":
            # Views of the same allocation share provenance in this bounded
            # capture. Pointer recycling and multiple command queues need
            # allocation-lifetime/queue IDs before long-term automatic use.
            resource = (fields["resource"], fields["allocation"])
            native = fields["native"]
            metadata[native] = dict(fields, identity=resource)
            flags = int(fields["flags"])
            if flags & 1:
                node["reads"].add(resource)
            if flags & 2:
                node["writes"].add(resource)
        elif kind == "render":
            node["render"] = fields
            profiles[tuple(fields[k] for k in ("width", "height", "colors", "samples")) + (fields["depth"] != "0",)] += 1
        elif kind == "upscale":
            upscales.append((encoder, fields))
        elif kind == "attachment" and fields.get("resolve") != "0":
            attachments += 1
    writers = collections.defaultdict(list)
    for encoder, node in sorted(nodes.items()):
        for resource in node["writes"]:
            writers[resource].append(encoder)
    roles = {}
    for encoder, event in upscales:
        for role in ("input", "depth", "motion", "output"):
            meta = metadata.get(event[role])
            if not meta:
                continue
            key = (role, meta["width"], meta["height"], meta["format"], meta["samples"], meta["layers"])
            result = roles.setdefault(key, {"observations": 0, "prior_writer_found": 0, "render_producers": set()})
            result["observations"] += 1
            prior = [w for w in writers[meta["identity"]] if w < encoder]
            if prior:
                result["prior_writer_found"] += 1
                if "render" in nodes[prior[-1]]:
                    result["render_producers"].add(prior[-1])
    return {
        "truncated": truncated,
        "render_encoders": sum(profiles.values()),
        "upscale_encoders": len(upscales),
        "explicit_resolve_attachments": attachments,
        "render_profiles": [dict(width=int(k[0]), height=int(k[1]), colors=int(k[2]), samples=int(k[3]), depth=k[4], count=v)
                            for k, v in profiles.most_common()],
        "upscale_resources": [dict(role=k[0], width=int(k[1]), height=int(k[2]), metal_format=int(k[3]), samples=int(k[4]), layers=int(k[5]),
                                   **{a: sorted(b)[:12] if isinstance(b, set) else b for a, b in v.items()}) for k, v in roles.items()],
        "limitations": ["Diagnostic only; no GPU pass durations or eye labels.",
                        "Single queue capture assumed; pointer/allocation reuse limits long-term identity.",
                        "Encoder merges and read-only depth tracking limit producer attribution.",
                        "Multiple attachments suggest a G-buffer but do not prove deferred rendering.",
                        "A dimension match does not establish foveation safety."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    with args.trace.open(errors="replace") as f:
        print(json.dumps(summarize(f), indent=2))


if __name__ == "__main__":
    main()
