#!/bin/zsh
# builds the loader, bundled plugins and the mvc-patch tool into ./build
# (needs Apple's command line tools: xcode-select --install)
set -e
cd "${0:A:h}"
mkdir -p build
CFLAGS=(-arch arm64 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -mmacosx-version-min=15.0)
[[ -n $MVC_WERROR ]] && CFLAGS+=(-Werror)   # CI: warnings are errors

# plugins/<name>/ -> GameData/plugins, scripts/<name>/ -> GameData/scripts
# (an optional NAME file inside the folder sets the output file name)
rm -f build/*.dylib(N) build/plugins/*.dylib(N) build/scripts/*.dylib(N)
xcrun clang $CFLAGS -dynamiclib \
  -install_name @executable_path/../Frameworks/macVCLoader.dylib \
  -fobjc-arc -framework Foundation -framework AppKit -framework AVFoundation -framework QuartzCore -framework JavaScriptCore -framework Security \
  loader/*.c loader/*.m loader/*.S -o build/macVCLoader.dylib
for kind in plugins scripts; do
  mkdir -p build/$kind
  for dir in $kind/*/(N); do
    out=${${dir%/}:t}
    [[ -f $dir/NAME ]] && out=$(<$dir/NAME)
    xcrun clang $CFLAGS -dynamiclib -install_name @rpath/$out.dylib \
      $dir/*.c build/macVCLoader.dylib -o build/$kind/$out.dylib
  done
done

# Drag-and-drop install: a proxy libmpg123.dylib, the way the Windows ASI Loader
# poses as dinput8.dll. macVC already loads Contents/Frameworks/libmpg123.dylib;
# the proxy re-exports the real one (shipped next to it as libmpg123.real.dylib)
# and weak-loads macVCLoader.dylib, so the game binary is never patched.
# the real library is macVC's own (LGPL mpg123), taken from an installed macVC.
MACVC=${MACVC_APP:-$HOME/Desktop/macVC.app}
REAL=$MACVC/Contents/Frameworks/libmpg123.real.dylib
[[ -f $REAL ]] || REAL=$MACVC/Contents/Frameworks/libmpg123.dylib
mkdir -p build/frameworks
# (with the loader installed, macVC's real library is already libmpg123.real.dylib;
# without it, libmpg123.dylib must be the real one, not a proxy left from another install)
if [[ -f $REAL ]] && { [[ $REAL == *.real.dylib ]] || ! otool -L "$REAL" | grep -q "libmpg123.real.dylib"; }; then
  cp "$REAL" build/frameworks/libmpg123.real.dylib
  install_name_tool -id @executable_path/../Frameworks/libmpg123.real.dylib build/frameworks/libmpg123.real.dylib 2>/dev/null
  cp build/macVCLoader.dylib build/frameworks/macVCLoader.dylib
  echo 'int mvc_mpg123_proxy;' > build/frameworks/proxy.c
  MINOS=$(otool -l build/frameworks/libmpg123.real.dylib | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
  xcrun clang -arch arm64 -dynamiclib -mmacosx-version-min=${MINOS:-15.0} build/frameworks/proxy.c \
    -install_name @executable_path/../Frameworks/libmpg123.dylib \
    -Wl,-reexport_library,build/frameworks/libmpg123.real.dylib -Wl,-weak_library,build/frameworks/macVCLoader.dylib \
    -o build/frameworks/libmpg123.dylib
  rm build/frameworks/proxy.c
else
  echo "note: no macVC at $MACVC (set MACVC_APP) - drag-and-drop files not built" >&2
fi

xcrun clang -arch arm64 -O2 -Wall -mmacosx-version-min=15.0 tools/mvc-patch.c -o build/mvc-patch
codesign -f -s - build/*.dylib build/*/*.dylib(N) build/mvc-patch 2>/dev/null
ls -l build build/plugins build/scripts
