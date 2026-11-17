#!/bin/zsh
# takes the loader back out of macVC.app and puts macVC's own libmpg123 back.
# (also restores an old patched binary from backup/ if there is one)
#   ./uninstall.sh [path/to/macVC.app]
set -e
cd "${0:A:h}"
APP=${1:-$HOME/Desktop/macVC.app}
APP=${APP%/}
BIN=$APP/Contents/MacOS/macVC
pgrep -x macVC >/dev/null && { echo "macVC is running - quit the game first."; exit 1; }
VER=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist" 2>/dev/null || echo unknown)
if [[ -f backup/macVC-$VER.orig ]]; then
  cp "backup/macVC-$VER.orig" "$BIN"
  echo "Restored the original macVC $VER binary."
else
  echo "No backup for macVC $VER - leaving the binary as is."
  echo "That's safe: the loader is linked as optional, so the game runs normally without it."
fi
FW=$APP/Contents/Frameworks
if [[ -f $FW/libmpg123.real.dylib ]]; then # undo the drag-and-drop / proxy install
  rm -f "$FW/libmpg123.dylib"
  mv "$FW/libmpg123.real.dylib" "$FW/libmpg123.dylib"
  install_name_tool -id @executable_path/../Frameworks/libmpg123.dylib "$FW/libmpg123.dylib" 2>/dev/null
  codesign -f -s - "$FW/libmpg123.dylib" 2>/dev/null
  echo "Restored macVC's libmpg123."
fi
rm -f "$FW/macVCLoader.dylib"
echo "Loader removed. Your GameData/plugins folder was left in place."
