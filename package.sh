#!/bin/zsh
# builds a release zip with prebuilt binaries: dist/macVC-loader-<version>.zip
# people install it with Install.command or drag and drop, no dev tools needed.
set -e
cd "${0:A:h}"
./build.sh >/dev/null
VER=$(grep -m1 'MVC_LOADER_VERSION' include/macvc.h | sed -E 's/.*"(.*)".*/\1/')
NAME=macVC-loader-$VER
STAGE=dist/$NAME
rm -rf "$STAGE" "dist/$NAME.zip"
mkdir -p "$STAGE/build/plugins" "$STAGE/build/scripts" "$STAGE/include"
mkdir -p "$STAGE/build/frameworks"
cp build/macVCLoader.dylib build/mvc-patch "$STAGE/build/"
cp build/frameworks/*.dylib "$STAGE/build/frameworks/"
cp build/plugins/*.dylib(N) "$STAGE/build/plugins/"
cp build/scripts/*.dylib(N) "$STAGE/build/scripts/"
cp install.sh uninstall.sh Install.command "Safe Mode.command" README.md CHANGELOG.md "$STAGE/"
cp include/macvc.h "$STAGE/include/"
osacompile -o "$STAGE/macVC Loader Installer.app" installer/Installer.applescript
# the drag-and-drop layout: these go straight into macVC.app/Contents
mkdir -p "$STAGE/Drag and Drop/Frameworks" "$STAGE/Drag and Drop/GameData/plugins" "$STAGE/Drag and Drop/GameData/scripts" "$STAGE/Drag and Drop/GameData/CLEO/CLEO_TEXT"
cp build/frameworks/*.dylib "$STAGE/Drag and Drop/Frameworks/"
cp build/plugins/*.dylib(N) "$STAGE/Drag and Drop/GameData/plugins/"
cp build/scripts/*.dylib(N) "$STAGE/Drag and Drop/GameData/scripts/"
chmod +x "$STAGE"/*.sh "$STAGE"/*.command "$STAGE/build/mvc-patch"
(cd dist && ditto -c -k --norsrc --noextattr --noqtn --keepParent "$NAME" "$NAME.zip")
(cd dist && shasum -a 256 "$NAME.zip" > "$NAME.zip.sha256")
echo "dist/$NAME.zip"
cat "dist/$NAME.zip.sha256"
