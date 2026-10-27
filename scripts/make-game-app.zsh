#!/bin/zsh
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Make "<Name> (Windows).app" for a Windows Steam game in the rig's prefix, to add to
# macOS Steam as a non-Steam game (Games > Add a Non-Steam Game to My Library). It runs
# the rig's run-windows-game.zsh, which starts the game on the macOS Steam client, so
# Steam also tracks it as the real app (docs/steam-games.md).
#
# Usage: [GAME_RUNNER=...] make-game-app.zsh APPID [DESTINATION, default ~/Applications]
#   GAME_RUNNER  the rig's launcher (default ~/Windows/run-windows-game.zsh)
set -euo pipefail
repo=${0:A:h:h}
(( $# >= 1 )) || { print -u2 "usage: make-game-app.zsh APPID [DESTINATION]"; exit 2; }
appid=$1 dest=${2:-$HOME/Applications}
runner=${GAME_RUNNER:-$HOME/Windows/run-windows-game.zsh}
[[ -x ${runner} ]] || { print -u2 "No launcher ${runner} (set GAME_RUNNER)"; exit 1; }

name=$(python3 "${repo}/tools/steam_games/steam_games.py" launch-spec "${appid}" --json |
  python3 -c 'import json,sys; print(json.load(sys.stdin)["name"])')
safe=${${name//:/}//\//-}
app="${dest}/${safe} (Windows).app"
art="$HOME/Library/Application Support/Steam/appcache/librarycache/${appid}/library_600x900.jpg"
work=$(mktemp -d); trap 'rm -rf "${work}"' EXIT

rm -rf "${app}"
mkdir -p "${app}/Contents/MacOS" "${app}/Contents/Resources"
cat > "${app}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>${safe}</string>
  <key>CFBundleDisplayName</key><string>${safe} (Windows)</string>
  <key>CFBundleIdentifier</key><string>org.mwxr.game.${appid}</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>LSUIElement</key><true/>
</dict>
</plist>
PLIST
# A login shell, so the launcher sees the same PATH as in Terminal.
cat > "${app}/Contents/MacOS/launch" <<LAUNCH
#!/bin/zsh -l
log=\$HOME/Library/Logs/macos-wine-xr/app-${appid}.log
mkdir -p "\${log:h}"
exec >>"\${log}" 2>&1
print "=== \$(date '+%Y-%m-%d %H:%M:%S') ${safe} (${appid})"
${(q)runner} ${appid} && exit 0
rc=\$?
osascript -e "display notification \"Exited with status \${rc}; see \${log}\" with title \"${safe}\""
exit \${rc}
LAUNCH
chmod +x "${app}/Contents/MacOS/launch"

if [[ -f ${art} ]]; then
  # the centre square of Steam's 600x900 cover, at the iconset's sizes
  set=${work}/AppIcon.iconset; mkdir -p "${set}"
  sips -s format png -c 600 600 "${art}" --out "${work}/square.png" >/dev/null
  for size in 16 32 128 256 512; do
    sips -z ${size} ${size} "${work}/square.png" --out "${set}/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) "${work}/square.png" --out "${set}/icon_${size}x${size}@2x.png" >/dev/null
  done
  iconutil -c icns "${set}" -o "${app}/Contents/Resources/AppIcon.icns"
fi
touch "${app}"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "${app}"
print "Made ${app}"
