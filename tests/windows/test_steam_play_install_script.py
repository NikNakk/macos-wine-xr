import importlib.util
from pathlib import Path
import unittest

module_path = Path(__file__).resolve().parents[2] / 'tools/steam_play/install_script.py'
spec = importlib.util.spec_from_file_location('install_script', module_path)
install_script = importlib.util.module_from_spec(spec)
spec.loader.exec_module(install_script)
parse_vdf = install_script.parse_vdf

# The shapes macOS Steam passed on 2026-10-28 (docs/steam-play.md), trimmed.
MR_ROBOT = r'''"evaluatorscript"
{
	"0"
	{
		"appid"		"20710"
		"mode"		"1"
		"language"		"english"
		"install_path"		"/Users/me/Library/Application Support/Steam/steamapps/common/MrRobot"
		"compat_installscript"
		{
			"Registry"
			{
				"HKEY_LOCAL_MACHINE\\SOFTWARE\\Moonpod\\MrRobot"
				{
					"string"
					{
						"installpath"		"%INSTALLDIR%"
					}
					"dword"
					{
						"Version"		"7"
					}
					"english"
					{
						"string" { "Language" "English" }
					}
					"german"
					{
						"string" { "Language" "Deutsch" }
					}
				}
			}
		}
	}
}'''

CANYON_CAPERS = r'''"evaluatorscript"
{
	"0"
	{
		"appid"		"228980"
		"mode"		"2"
		"install_path"		"/Users/me/Library/Application Support/Steam/steamapps/common/Steamworks Shared"
		"compat_installscript"
		{
			"run process"
			{
				"dxsetup"
				{
					"hasrunkey"		"HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam\\Apps\\CommonRedist\\DirectX\\Jun2010"
					"process 1"		"%INSTALLDIR%\\_CommonRedist\\DirectX\\Jun2010\\DXSETUP.exe"
					"command 1"		"/silent"
				}
			}
		}
	}
}'''


class InstallScriptTests(unittest.TestCase):
    def test_registry_step(self):
        reg, log = install_script.convert(parse_vdf(MR_ROBOT))
        self.assertTrue(reg.startswith('Windows Registry Editor Version 5.00\r\n'))
        self.assertIn('[HKEY_LOCAL_MACHINE\\SOFTWARE\\Moonpod\\MrRobot]', reg)
        self.assertIn('"installpath"="Z:\\\\Users\\\\me\\\\Library\\\\Application Support\\\\Steam'
                      '\\\\steamapps\\\\common\\\\MrRobot"', reg)
        self.assertIn('"Version"=dword:00000007', reg)
        self.assertIn('"Language"="English"', reg)
        self.assertNotIn('Deutsch', reg)
        self.assertTrue(any("skipped 'german'" in line for line in log))

    def test_installers_are_skipped(self):
        reg, log = install_script.convert(parse_vdf(CANYON_CAPERS))
        self.assertIsNone(reg)
        self.assertEqual(len(log), 1)
        self.assertIn("skipped installer 'dxsetup'", log[0])

    def test_unknown_root_is_skipped(self):
        script = MR_ROBOT.replace('HKEY_LOCAL_MACHINE', 'HKEY_NOWHERE')
        reg, log = install_script.convert(parse_vdf(script))
        self.assertIsNone(reg)
        self.assertTrue(any('unknown registry root' in line for line in log))


if __name__ == '__main__':
    unittest.main()
