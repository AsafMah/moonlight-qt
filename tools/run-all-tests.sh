#!/usr/bin/env bash
# Builds and runs every unit test CI runs, against a checkout. The list comes
# from the checkout's .github/workflows/build.yml (its run-test.bat lines).
# Usage: run-all-tests.sh [checkout]   (default E:/Code/moonlight-qt)
# overlay_button_native_wake fails under offscreen; run it on its own.
src=${1:-E:/Code/moonlight-qt}
srcunix=$(cygpath -u "$src")
tests=$(grep -oE "run-test.bat [a-z_0-9]+ [a-z_0-9]+" "$srcunix/.github/workflows/build.yml" |
        awk '{print $2":"$3}' | sort -u)
mkdir -p /e/Code/moonlight-tools/logs/tests
for t in $tests; do
    dir=${t%%:*}; target=${t##*:}
    cmd //c "E:\\Code\\moonlight-tools\\test-local.bat $dir $target $src" > /e/Code/moonlight-tools/logs/tests/$dir.log 2>&1
    echo "$dir: exit $?"
done
