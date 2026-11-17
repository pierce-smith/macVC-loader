#!/bin/zsh
# offline tests: the hook engine unit tests and the parser fuzzers, under
# AddressSanitizer + UndefinedBehaviorSanitizer. no game needed.
#   tests/run.sh            hook tests + every fuzzer, 20000 runs each
#   tests/run.sh 200000     longer fuzzing
#   tests/run.sh hook       only the hook tests
set -e
setopt pipefail   # a failing test must fail the run even when its output is filtered
cd "${0:A:h}/.."
OUT=${TMPDIR:-/tmp}/macvc-tests
mkdir -p $OUT
SAN=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
CC=(xcrun clang -arch arm64 -g -O1 -Wall -Wno-unused-function -mmacosx-version-min=15.0)
ITERS=20000
ONLY=""
[[ $1 == <-> ]] && ITERS=$1
[[ -n $1 && $1 != <-> ]] && ONLY=$1

echo "== hook engine"
$CC $SAN -o $OUT/hooktest tests/hook/hooktest.c tests/hook/funcs.S loader/hook.c loader/midhook.S loader/log.c
$OUT/hooktest | grep -v '^\['
$CC -o $OUT/hooktest_plain tests/hook/hooktest.c tests/hook/funcs.S loader/hook.c loader/midhook.S loader/log.c
$OUT/hooktest_plain | grep -v '^\[' | sed 's/^/without sanitizers: /'
[[ $ONLY == hook ]] && exit 0

FZ=(tests/fuzz/driver.c tests/fuzz/stubs.c)
typeset -A SRC
SRC[ini]="loader/menu.c loader/text.c"
SRC[fxt]="loader/text.c"
SRC[merge]="loader/modmerge.c"
SRC[col]="loader/modmerge.c"
SRC[txd]="loader/txdbuild.c"
SRC[gxt]="loader/text.c loader/modmerge.c loader/txdbuild.c loader/menu.c"
SRC[cleo_format]="loader/menu.c loader/text.c"
FW=(-framework CoreFoundation -framework JavaScriptCore)
for t in ini fxt merge col txd gxt cleo_format; do
  [[ -n $ONLY && $ONLY != $t ]] && continue
  echo "== fuzz $t"
  $CC $SAN -o $OUT/fuzz_$t tests/fuzz/fuzz_$t.c $FZ ${=SRC[$t]} $FW
  (cd $OUT && ./fuzz_$t -n $ITERS -s $RANDOM)
done
if [[ -z $ONLY || $ONLY == js ]]; then
  echo "== JavaScript prelude"
  $CC $SAN -o $OUT/prelude_test tests/js/prelude_test.c tests/fuzz/stubs.c -framework JavaScriptCore
  $OUT/prelude_test
fi

# texture packs against the real game archive, when a macVC is installed
# (a scratch GameData that links to the game's folders; macVC.app is never written)
GD=${MACVC_APP:-$HOME/Desktop/macVC.app}/Contents/Resources/GameData
if [[ ( -z $ONLY || $ONLY == texpack ) && -f $GD/models/gta3.img ]]; then
  echo "== texture packs (game files from $GD)"
  rm -rf $OUT/tp && mkdir -p $OUT/tp/GameData/modloader/TestPack/textures/sub
  for d in models data txd; do [[ -e $GD/$d ]] && ln -s "$GD/$d" $OUT/tp/GameData/$d; done
  $CC $SAN -o $OUT/texpack_test tests/texpack/texpack_test.c tests/fuzz/stubs.c loader/text.c loader/modmerge.c \
    loader/txdbuild.c loader/menu.c $FW
  $OUT/texpack_test $OUT/tp/GameData
  rm -rf $OUT/tp
fi
echo "all tests passed"
