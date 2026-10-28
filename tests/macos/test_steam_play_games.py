import importlib.util
from pathlib import Path
import tempfile
import unittest

module_path = Path(__file__).resolve().parents[2] / 'tools/steam_play/steam_play_games.py'
spec = importlib.util.spec_from_file_location('steam_play_games', module_path)
games = importlib.util.module_from_spec(spec)
spec.loader.exec_module(games)


def app(name, oslist, kind='game'):
    return {'common': {'name': name, 'oslist': oslist, 'type': kind}}


APPS = {
    1: app('Windows only', 'windows'),
    2: {'common': {'name': 'Windows and macOS', 'oslist': 'windows,macos', 'type': 'game', 'osextended': 'macos64'}},
    3: app('macOS only', 'macos'),
    4: app('Windows and Linux', 'windows,linux'),
    5: app('A Windows tool', 'windows', kind='tool'),
    6: {'common': {'name': 'VR, Windows and macOS', 'oslist': 'windows,macos', 'type': 'game',
                   'openvrsupport': '1'}},
    7: {'common': {'name': 'VR, macOS only', 'oslist': 'macos', 'type': 'game', 'openxrsupport': '1'}},
    8: {'common': {'name': '32-bit macOS build', 'oslist': 'windows,macos', 'type': 'game'}},
    9: {'common': {'name': '32-bit macOS only', 'oslist': 'macos', 'type': 'game'}},
    228980: app('Steamworks Common Redistributables', 'windows'),
}


class SteamPlayGamesTests(unittest.TestCase):
    def test_windows_only_games_are_mapped(self):
        self.assertEqual(games.windows_only(APPS, set(APPS)), {1, 4})

    def test_games_with_a_macos_build_need_listing(self):
        mapped = games.mapping(APPS, set(APPS), listed={2}, excluded=set())
        self.assertEqual(mapped, {1: 'windows-only', 4: 'windows-only', 6: 'vr', 8: 'mac-32-bit', 2: 'listed'})

    def test_games_with_only_a_32_bit_macos_build_are_mapped(self):
        self.assertEqual(games.mac_32_bit(APPS, set(APPS)), {6, 8})

    def test_vr_games_with_a_windows_build_are_mapped(self):
        self.assertEqual(games.vr_games(APPS, set(APPS)), {6})
        self.assertEqual(games.mapping(APPS, set(APPS), listed=set(), excluded={6}),
                         {1: 'windows-only', 4: 'windows-only', 8: 'mac-32-bit'})

    def test_exclusions_win(self):
        mapped = games.mapping(APPS, set(APPS), listed={2}, excluded={1, 2, 6, 8})
        self.assertEqual(mapped, {4: 'windows-only'})

    def test_only_owned_games_are_mapped(self):
        self.assertEqual(games.mapping(APPS, {2, 3}, listed=set(), excluded=set()), {})

    def test_games_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'games'
            path.write_text('# comment\n1256230   # Hyperbolica\n-20710  # Mr. Robot\nnonsense\n\n')
            self.assertEqual(games.read_games(path), ({1256230}, {20710}))
            self.assertEqual(games.read_games(Path(directory) / 'missing'), (set(), set()))


if __name__ == '__main__':
    unittest.main()
