#!/usr/bin/env python3
"""Export native-size resolve-first images, difference masks and thin-feature crops."""
import argparse
from pathlib import Path
from visualize_dxmt_depth_probe import read_ppm, png


def crop(data, width, left, top, size, scale=3):
    return b"".join(b"".join(data[((top+y)*width+left+x)*3:((top+y)*width+left+x)*3+3]*scale
                            for x in range(size)) for y in range(size) for _ in range(scale))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--samples", type=int, default=2)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = f"depth-{args.size}-{args.samples}-"
    width, height, baseline = read_ppm(args.source/(prefix+"resolve-baseline.ppm"))
    for name in ["baseline", "rate025", "rate050", "rate075", "rate100"]:
        w, h, data = read_ppm(args.source/(prefix+"resolve-"+name+".ppm"))
        if (w, h) != (width, height):
            raise ValueError("Mismatching image sizes")
        png(args.output/(name+".png"), w, h, data)
        length = width//5
        patch = crop(data, width, width//40, height//3, length)
        png(args.output/(name+"-thin-crop.png"), length*3, length*3, patch)
        if name != "baseline":
            mask = bytearray(w*h*3)
            for i in range(w*h):
                if any(abs(data[3*i+c]-baseline[3*i+c]) > 3 for c in range(3)):
                    mask[3*i:3*i+3] = b"\xff\x40\x10"
            png(args.output/(name+"-difference.png"), w, h, mask)


if __name__ == "__main__":
    main()
