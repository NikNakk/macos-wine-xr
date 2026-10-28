#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Installs Steam Play for macOS Steam (docs/steam-play.md): the compat gate library and
# the macos-wine-xr Steam Play tool in ~/Library/Application Support/macos-wine-xr/steam-play,
# and the launcher app ~/Applications/Steam Play.app. Valve's Steam.app is not touched.
#
# Usage: install-steam-play.zsh [--uninstall]
#   STEAM_PLAY_RUNNER  the rig's run-windows-game.zsh wrapper that games are started with
#                      (default ~/Windows/run-windows-game.zsh)
#   STEAM_PLAY_APP     where the launcher goes (default ~/Applications/Steam Play.app)
set -euo pipefail
repo=${0:A:h:h}
payload="$HOME/Library/Application Support/macos-wine-xr/steam-play"
app=${STEAM_PLAY_APP:-$HOME/Applications/Steam Play.app}
runner=${STEAM_PLAY_RUNNER:-$HOME/Windows/run-windows-game.zsh}
build=${repo}/build-in-process/steam-play

if [[ ${1:-} == --uninstall ]]; then
  rm -rf "${app}" "${payload}"
  # Leaves ~/Library/Logs/macos-wine-xr/steam-play.log and steam-compat-gate.log.
  print "Removed ${app} and ${payload}. Steam started from Steam.app has Steam Play off."
  exit 0
fi
[[ -x ${runner} ]] || { print -u2 "No runner at ${runner} (set STEAM_PLAY_RUNNER)"; exit 1; }

mkdir -p "${build}"
clang -Wall -Wextra -O2 -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 -dynamiclib \
  -install_name @rpath/libmwxr_steam_compat_gate.dylib \
  "${repo}/src/macos/steam_compat_gate.c" -o "${build}/libmwxr_steam_compat_gate.dylib"
clang -Wall -Wextra -O2 -arch arm64 -mmacosx-version-min=13.0 \
  "${repo}/src/macos/steam_play_launcher.c" -o "${build}/steam-play"
clang -Wall -Wextra -O2 -arch arm64 -DMWXR_GATE_SCAN_MAIN \
  "${repo}/src/macos/steam_compat_gate.c" -o "${build}/steam-compat-gate-scan"
codesign -f -s - "${build}/libmwxr_steam_compat_gate.dylib" "${build}/steam-play" \
  "${build}/steam-compat-gate-scan" 2>/dev/null

tool="${payload}/compatibilitytools.d/macos-wine-xr"
mkdir -p "${tool}"
cp -c "${build}/libmwxr_steam_compat_gate.dylib" "${build}/steam-compat-gate-scan" "${payload}/"
cp -c "${repo}/tools/steam_play/compatibilitytool.vdf" "${repo}/tools/steam_play/toolmanifest.vdf" \
  "${repo}/tools/steam_play/mwxr-steam-play" "${tool}/"
chmod +x "${tool}/mwxr-steam-play"
print -r -- "${runner}" > "${tool}/runner"
# The games Steam Play is used for: only these get the Windows build and this tool.
# Kept across reinstalls; edit it, then restart Steam through Steam Play.app.
if [[ ! -f ${payload}/games ]]; then
  cat > "${payload}/games" <<'GAMES'
# App IDs macOS Steam installs as Windows games and starts through macos-wine-xr, one per
# line. Restart Steam through Steam Play.app after a change. A game that also has a macOS
# build switches to its Windows build when listed, and back when removed.
1256230   # Hyperbolica
GAMES
fi

rm -rf "${app}"
mkdir -p "${app}/Contents/MacOS" "${app}/Contents/Resources"
cp -c "${build}/steam-play" "${app}/Contents/MacOS/steam-play"
steam_icon=/Applications/Steam.app/Contents/Resources/Steam.icns
[[ -f ${steam_icon} ]] && cp -c "${steam_icon}" "${app}/Contents/Resources/Steam.icns"
cat > "${app}/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>steam-play</string>
	<key>CFBundleIdentifier</key>
	<string>org.macos-wine-xr.steam-play</string>
	<key>CFBundleName</key>
	<string>Steam Play</string>
	<key>CFBundleDisplayName</key>
	<string>Steam Play</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleIconFile</key>
	<string>Steam</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0</string>
	<key>LSArchitecturePriority</key>
	<array>
		<string>arm64</string>
	</array>
	<key>LSMinimumSystemVersion</key>
	<string>13.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
PLIST
codesign -f -s - "${app}"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "${app}" || true

# Check the installed Steam client against the gate now rather than at the next launch.
client="$HOME/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib"
if [[ -f ${client} ]]; then
  thin=$(mktemp -d "${TMPDIR:-/tmp}/steam-play.XXXXXX")
  lipo -thin arm64 "${client}" -output "${thin}/steamclient.dylib" 2>/dev/null || cp -c "${client}" "${thin}/steamclient.dylib"
  MWXR_STEAM_COMPAT_GATE_LOG=${thin}/scan.log "${payload}/steam-compat-gate-scan" "${thin}/steamclient.dylib"
  print -r -- "Steam client check: $(cat "${thin}/scan.log" 2>/dev/null)"
  rm -rf "${thin}"
fi
print "Installed ${payload} and ${app}. Quit Steam, then open Steam Play to start it with Steam Play on."
