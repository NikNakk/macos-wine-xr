#!/usr/bin/env python3
"""Convert diagnostic PPM outputs to PNG and export a pixel-difference mask."""
import argparse
from pathlib import Path
import struct
import zlib


def read_ppm(path):
    magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    width, height = map(int, dimensions.split())
    if magic != b"P6" or maximum != b"255" or len(pixels) != width*height*3:
        raise ValueError(f"Invalid probe PPM: {path}")
    return width, height, pixels


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    rows = b"".join(b"\0"+pixels[y*width*3:(y+1)*width*3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))+
                     chunk(b"IDAT", zlib.compress(rows))+chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--samples", type=int, default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = f"depth-{args.size}-{args.samples}-"
    width, height, baseline = read_ppm(args.source/(prefix+"baseline.ppm"))
    for name in ["baseline", "nearest", "bilinear", "min", "nearest-full-depth", "bilinear-full-depth", "min-full-depth"]:
        w, h, data = read_ppm(args.source/(prefix+name+".ppm"))
        if (w, h) != (width, height):
            raise ValueError("Mismatching image sizes")
        png(args.output/(name+".png"), w, h, data)
        if name != "baseline":
            mask = bytearray(w*h*3)
            for i in range(w*h):
                bad = any(abs(data[3*i+c]-baseline[3*i+c]) > 3 for c in range(3))
                if bad:
                    mask[3*i:3*i+3] = b"\xff\x40\x10"
            png(args.output/(name+"-difference.png"), w, h, mask)


if __name__ == "__main__":
    main()
