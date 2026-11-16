# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/steam_play/display_modes.py: reads this Mac's modes, and restore leaves an
unchanged display alone. It does not change any display."""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "steam_play"))
import display_modes  # noqa: E402


class DisplayModesTest(unittest.TestCase):
    def test_current_has_main_display(self):
        modes = display_modes.current()
        self.assertTrue(modes)
        for mode in modes.values():
            self.assertGreater(mode["width"], 0)
            self.assertGreaterEqual(mode["pixel_width"], mode["width"])

    def test_hidpi_modes_listed(self):
        main = display_modes.cg.CGMainDisplayID() if hasattr(display_modes.cg, "CGMainDisplayID") else 1
        modes = display_modes.cg.CGDisplayCopyAllDisplayModes(main, display_modes.all_modes_options())
        count = display_modes.cf.CFArrayGetCount(modes)
        listed = [display_modes.describe(display_modes.cf.CFArrayGetValueAtIndex(modes, i)) for i in range(count)]
        self.assertTrue(any(m["pixel_width"] > m["width"] for m in listed), "no HiDPI modes listed")

    def test_restore_unchanged_is_a_no_op(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "modes.json")
            self.assertEqual(display_modes.main(["x", "save", path]), 0)
            with open(path) as f:
                self.assertEqual(display_modes.restore(json.load(f)), 0)

    def test_unknown_display_ignored(self):
        self.assertEqual(display_modes.restore({"0:0:0": {"width": 1, "height": 1, "pixel_width": 1,
                                                          "pixel_height": 1, "refresh": 1, "mode_id": 1}}), 0)


if __name__ == "__main__":
    unittest.main()
