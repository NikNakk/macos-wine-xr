#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Save the Mac's display modes before a game and put them back after it.

A full-screen Windows game sets the display's mode through Wine (winemac). Wine sets it
back when the game does, but not when the game crashes: macOS keeps the mode after the
process has gone, so a crash on quit leaves the Mac's display unscaled at its full pixel
size (docs/steam-play.md, The Mac's display mode). run-windows-game.zsh saves the modes
before the game starts and restores any that changed once it has exited.

    display_modes.py save FILE      # every online display's current mode, as JSON
    display_modes.py restore FILE   # set each saved display back to its saved mode
    display_modes.py show           # the current modes
"""
import ctypes
import ctypes.util
import json
import sys

cg = ctypes.CDLL(ctypes.util.find_library("CoreGraphics"))
cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
vp, u32 = ctypes.c_void_p, ctypes.c_uint32
for name, res, args in [
    ("CGGetOnlineDisplayList", ctypes.c_int32, [u32, ctypes.POINTER(u32), ctypes.POINTER(u32)]),
    ("CGDisplayCopyDisplayMode", vp, [u32]),
    ("CGDisplayCopyAllDisplayModes", vp, [u32, vp]),
    ("CGDisplayModeGetIODisplayModeID", ctypes.c_int32, [vp]),
    ("CGDisplayModeGetWidth", ctypes.c_size_t, [vp]),
    ("CGDisplayModeGetHeight", ctypes.c_size_t, [vp]),
    ("CGDisplayModeGetPixelWidth", ctypes.c_size_t, [vp]),
    ("CGDisplayModeGetPixelHeight", ctypes.c_size_t, [vp]),
    ("CGDisplayModeGetRefreshRate", ctypes.c_double, [vp]),
    ("CGDisplayModeRelease", None, [vp]),
    ("CGDisplaySetDisplayMode", ctypes.c_int32, [u32, vp, vp]),
    ("CGDisplayVendorNumber", u32, [u32]),
    ("CGDisplayModelNumber", u32, [u32]),
    ("CGDisplaySerialNumber", u32, [u32]),
]:
    f = getattr(cg, name)
    f.restype, f.argtypes = res, args
cf.CFArrayGetCount.restype, cf.CFArrayGetCount.argtypes = ctypes.c_long, [vp]
cf.CFArrayGetValueAtIndex.restype, cf.CFArrayGetValueAtIndex.argtypes = vp, [vp, ctypes.c_long]
cf.CFRelease.argtypes = [vp]
cf.CFDictionaryCreate.restype = vp
cf.CFDictionaryCreate.argtypes = [vp, ctypes.POINTER(vp), ctypes.POINTER(vp), ctypes.c_long, vp, vp]


def displays():
    ids, count = (u32 * 16)(), u32(0)
    if cg.CGGetOnlineDisplayList(16, ids, ctypes.byref(count)) != 0:
        return []
    return list(ids[: count.value])


def describe(mode):
    return {
        "mode_id": cg.CGDisplayModeGetIODisplayModeID(mode),
        "width": cg.CGDisplayModeGetWidth(mode),
        "height": cg.CGDisplayModeGetHeight(mode),
        "pixel_width": cg.CGDisplayModeGetPixelWidth(mode),
        "pixel_height": cg.CGDisplayModeGetPixelHeight(mode),
        "refresh": round(cg.CGDisplayModeGetRefreshRate(mode), 3),
    }


def key(display):
    # Display IDs can change between connections; vendor, model and serial do not.
    return "%x:%x:%x" % (cg.CGDisplayVendorNumber(display), cg.CGDisplayModelNumber(display),
                         cg.CGDisplaySerialNumber(display))


def current():
    out = {}
    for d in displays():
        mode = cg.CGDisplayCopyDisplayMode(d)
        if mode:
            out[key(d)] = dict(describe(mode), display=d)
            cg.CGDisplayModeRelease(mode)
    return out


def all_modes_options():
    # kCGDisplayShowDuplicateLowResolutionModes: list the HiDPI modes too.
    k = vp.in_dll(cg, "kCGDisplayShowDuplicateLowResolutionModes").value
    keys, values = (vp * 1)(k), (vp * 1)(vp.in_dll(cf, "kCFBooleanTrue").value)
    callbacks = lambda name: ctypes.addressof(ctypes.c_char.in_dll(cf, name))
    options = cf.CFDictionaryCreate(None, keys, values, 1, callbacks("kCFTypeDictionaryKeyCallBacks"),
                                    callbacks("kCFTypeDictionaryValueCallBacks"))
    return options


SAME = ("width", "height", "pixel_width", "pixel_height", "refresh")


def restore(saved):
    now = current()
    options = all_modes_options()
    changed = 0
    for k, want in saved.items():
        have = now.get(k)
        if not have or all(have[f] == want[f] for f in SAME):
            continue
        d = have["display"]
        modes = cg.CGDisplayCopyAllDisplayModes(d, options)
        match = None
        for i in range(cf.CFArrayGetCount(modes) if modes else 0):
            m = cf.CFArrayGetValueAtIndex(modes, i)
            desc = describe(m)
            if all(desc[f] == want[f] for f in SAME):
                match = m
                if desc["mode_id"] == want["mode_id"]:
                    break
        if match is None:
            print(f"display {d}: saved mode {want['width']}x{want['height']} not offered any more", file=sys.stderr)
        elif cg.CGDisplaySetDisplayMode(d, match, None) == 0:
            print(f"display {d}: restored {want['width']}x{want['height']} "
                  f"({want['pixel_width']}x{want['pixel_height']} pixels) from "
                  f"{have['width']}x{have['height']} ({have['pixel_width']}x{have['pixel_height']})")
            changed += 1
        else:
            print(f"display {d}: could not restore {want['width']}x{want['height']}", file=sys.stderr)
        if modes:
            cf.CFRelease(modes)
    cf.CFRelease(options)
    return changed


def main(argv):
    if len(argv) == 2 and argv[1] == "show":
        print(json.dumps(current(), indent=2))
    elif len(argv) == 3 and argv[1] == "save":
        with open(argv[2], "w") as f:
            json.dump(current(), f)
    elif len(argv) == 3 and argv[1] == "restore":
        try:
            with open(argv[2]) as f:
                saved = json.load(f)
        except (OSError, ValueError) as e:
            print(f"display_modes: {e}", file=sys.stderr)
            return 1
        restore(saved)
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
