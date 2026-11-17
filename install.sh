#!/bin/zsh
# installs the loader and bundled plugins into macVC.app
#   ./install.sh [path/to/macVC.app]     (default ~/Desktop/macVC.app)
# rerun it after a macVC update, updates replace the Frameworks folder.
# release zips have everything prebuilt in ./build so you don't need dev tools.
set -e
cd "${0:A:h}"
APP=${1:-$HOME/Desktop/macVC.app}
APP=${APP%/}
BIN=$APP/Contents/MacOS/macVC
GD=$APP/Contents/Resources/GameData
LOADER_PATH=@executable_path/../Frameworks/macVCLoader.dylib

if [[ ! -x $BIN ]]; then
  echo "macVC not found at: $APP"
  echo "Usage: ./install.sh /path/to/macVC.app"
  exit 1
fi
if pgrep -x macVC >/dev/null; then
  echo "macVC is running - quit the game first."
  exit 1
fi

# build from source when developer tools are present, otherwise use the prebuilt files
if [[ -f loader/loader.c ]] && xcrun --find clang >/dev/null 2>&1; then
  echo "Building..."
  ./build.sh >/dev/null
fi
for f in build/macVCLoader.dylib build/mvc-patch build/frameworks/libmpg123.dylib; do
  [[ -f $f ]] || { echo "Missing $f - download a release zip, or install developer tools: xcode-select --install"; exit 1; }
done

VER=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$APP/Contents/Info.plist" 2>/dev/null || echo unknown)
echo "macVC $VER at $APP"

# verify the game binary against macVC's version.sha, when it sits next to the app
SHAFILE=${APP:h}/version.sha
if [[ -f $SHAFILE ]] && ! build/mvc-patch check "$BIN" "$LOADER_PATH"; then
  WANT=$(tr -d ' \n' < "$SHAFILE" | cut -c1-64)
  HAVE=$(shasum -a 256 "$BIN" | cut -c1-64)
  if [[ $WANT != "$HAVE" ]]; then
    echo "The macVC binary does not match version.sha - it may be damaged or modified."
    echo "Re-download macVC, or delete version.sha to install anyway."
    exit 1
  fi
  echo "macVC binary verified against version.sha"
fi

# every game function the loader and plugins use must exist in this macVC build
echo "Checking compatibility with this macVC build..."
if ! build/mvc-patch compat "$BIN" build/macVCLoader.dylib build/plugins/*.dylib(N) build/scripts/*.dylib(N); then
  echo
  echo "WARNING: some game functions are missing in this macVC build (listed above)."
  echo "The loader will turn off whatever depends on them instead of crashing."
fi

# rm before cp: a running process keeps the old file mapped instead of crashing
rm -f "$APP/Contents/Frameworks/macVCLoader.dylib"
cp build/macVCLoader.dylib "$APP/Contents/Frameworks/"
for kind in plugins scripts; do
  mkdir -p "$GD/$kind"
  for f in build/$kind/*.dylib(N); do
    rm -f "$GD/$kind/${f:t}"
    cp "$f" "$GD/$kind/"
    echo "  $kind/${f:t}"
  done
done

# the loader is loaded through a proxy libmpg123.dylib (the game binary is never
# touched): macVC's own libmpg123 becomes libmpg123.real.dylib and the proxy,
# which re-exports it and loads the loader, takes its place.
FW=$APP/Contents/Frameworks
if [[ -f $FW/libmpg123.dylib ]] && ! otool -L "$FW/libmpg123.dylib" | grep -q libmpg123.real.dylib; then
  rm -f "$FW/libmpg123.real.dylib"
  mv "$FW/libmpg123.dylib" "$FW/libmpg123.real.dylib"
  install_name_tool -id @executable_path/../Frameworks/libmpg123.real.dylib "$FW/libmpg123.real.dylib" 2>/dev/null
  echo "macVC's libmpg123 kept as libmpg123.real.dylib"
elif [[ ! -f $FW/libmpg123.real.dylib && -f build/frameworks/libmpg123.real.dylib ]]; then
  cp build/frameworks/libmpg123.real.dylib "$FW/"
fi
[[ -f $FW/libmpg123.real.dylib ]] || { echo "libmpg123 not found in macVC.app - is this macVC?"; exit 1; }
rm -f "$FW/libmpg123.dylib"
cp build/frameworks/libmpg123.dylib "$FW/"
codesign -f -s - "$FW/libmpg123.dylib" "$FW/libmpg123.real.dylib" "$FW/macVCLoader.dylib" "$GD"/plugins/*.dylib(N) "$GD"/scripts/*.dylib(N) 2>/dev/null
# files copied out of a downloaded zip carry the quarantine flag; the game would refuse to load them
xattr -dr com.apple.quarantine "$FW" "$GD/plugins" "$GD/scripts" 2>/dev/null || true

echo
echo "Installed. Launch macVC and press the ~ key (left of 1) for the console."
echo "Plugins: $GD/plugins"
echo "Log:     $GD/macVCLoader.log"
