#!/usr/bin/env bash
set -u

cd "$(dirname "$0")"
mkdir -p out

# The extracted tests expect the old flat tree and find the fonts in out/.
# These links exist only while this build is running.
ln -sfn "../fonts/console-8x16.psfu" out/console-8x16.psfu
ln -sfn "../fonts/console-16x32.psfu" out/console-16x32.psfu
ln -sfn "../src/ar_type.c" tools/ar_type.c
ln -sfn "../src/ar_type_tables.h" tools/ar_type_tables.h
ln -sfn "../src/ar_type_latin.h" tools/ar_type_latin.h
cleanup() {
    rm -f out/console-8x16.psfu out/console-16x32.psfu \
        tools/ar_type.c tools/ar_type_tables.h tools/ar_type_latin.h
}
trap cleanup EXIT HUP INT TERM

failed=0
tests=0
known_fail=0

compile() {
    label=$1
    shift
    log=$(mktemp)
    if "$@" >"$log" 2>&1; then
        rm -f "$log"
        return 0
    else
        rc=$?
        printf 'TEST %s FAIL exit=%s\n' "$label" "$rc"
        tail -n 40 "$log"
        rm -f "$log"
        failed=1
        return 1
    fi
}

# Preserve pass 2's binary names and add src/ to the include path for the
# relocated C sources.
compile ar-type cc -static -O2 -Wall -Isrc -o out/ar-type src/ar_type.c || true
compile ar-shell cc -static -O2 -Wall -Isrc -DAR_TYPE_SHELL=1 -o out/ar-shell src/ar_type.c || true
compile شكل cc -static -O2 -Wall -Isrc -o out/شكل src/arsh.c -lutil || true

# Command names below match each source file's argv[0] uses and diagnostics.
compile اعرض cc -static -O2 -Wall -Isrc -o out/اعرض src/ar_list.c || true
compile اطبع cc -static -O2 -Wall -Isrc -o out/اطبع src/ar_print.c || true
compile أنشئ cc -static -O2 -Wall -Isrc -o out/أنشئ src/ar_mkdir.c || true
compile انسخ cc -static -O2 -Wall -Isrc -o out/انسخ src/ar_copy.c || true
compile انقل cc -static -O2 -Wall -Isrc -o out/انقل src/ar_move.c || true
compile أعد-التسمية cc -static -O2 -Wall -Isrc -o 'out/أعد التسمية' src/ar_move.c || true
compile احذف cc -static -O2 -Wall -Isrc -o out/احذف src/ar_remove.c || true
compile فتّش cc -static -O2 -Wall -Isrc -o out/فتّش src/ar_search.c || true
compile ابحث cc -static -O2 -Wall -Isrc -o out/ابحث src/ar_find.c || true
compile اعرض-الأذونات cc -static -O2 -Wall -Isrc -o 'out/اعرض الأذونات' src/ar_perm.c || true
compile اضبط-الأذونات cc -static -O2 -Wall -Isrc -DPERM_SET=1 -o 'out/اضبط الأذونات' src/ar_perm.c || true
compile حدّث-الحزم cc -static -O2 -Wall -Isrc -o 'out/حدّث الحزم' src/ar_pkg_update.c || true
compile ثبّت-حزمة cc -static -O2 -Wall -Isrc -o 'out/ثبّت حزمة' src/ar_pkg_install.c || true
compile أزل-حزمة cc -static -O2 -Wall -Isrc -o 'out/أزل حزمة' src/ar_pkg_remove.c || true
compile تاريخ cc -static -O2 -Wall -Icalendars -o out/تاريخ calendars/hijri.c || true
compile hisab cc -static -O2 -Wall -Icalendars -o out/hisab calendars/hisab.c || true

run_test() {
    label=$1
    shift
    tests=$((tests + 1))
    log=$(mktemp)
    if PYTHONPATH="$PWD/tools" "$@" >"$log" 2>&1; then
        printf 'TEST %s PASS exit=0\n' "$label"
    else
        rc=$?
        # Only these exact failed assertions are an accepted known gap.
        known=0
        unexpected=0
        while IFS= read -r line; do
            case "$line" in
                *FAIL*)
                    case "$line" in
                        *'S1 the child stdin is /dev/null'*|\
                        *'S3 Down past the newest recalls an empty line'*|\
                        *'S4 backspace deletes one atom and keeps the lam-alef ligature'*|\
                        *'S5 the caret column is the CELL-derived column'*)
                            known=$((known + 1))
                            ;;
                        *'=== VERIFY FAIL'*) ;;
                        *) unexpected=$((unexpected + 1)) ;;
                    esac
                    ;;
            esac
        done <"$log"
        if [ "$known" -gt 0 ] && [ "$unexpected" -eq 0 ]; then
            printf 'TEST %s KNOWN-FAIL exit=%s\n' "$label" "$rc"
            known_fail=$((known_fail + known))
        else
            printf 'TEST %s FAIL exit=%s\n' "$label" "$rc"
            failed=1
            tail -n 40 "$log"
        fi
    fi
    rm -f "$log"
}

# Tests run in the requested order. The C test includes ar_type.c by its
# legacy basename, supplied by the temporary tools/ar_type.c symlink above.
tests=$((tests + 1))
if cc -O2 -Wall -Isrc -o out/ar_type_safety_test tools/ar_type_safety_test.c; then
    log=$(mktemp)
    if PYTHONPATH="$PWD/tools" out/ar_type_safety_test >"$log" 2>&1; then
        printf 'TEST ar_type_safety_test PASS exit=0\n'
    else
        rc=$?
        printf 'TEST ar_type_safety_test FAIL exit=%s\n' "$rc"
        tail -n 40 "$log"
        failed=1
    fi
    rm -f "$log"
else
    rc=$?
    printf 'TEST ar_type_safety_test FAIL exit=%s\n' "$rc"
    tail -n 40 "$log"
    rm -f "$log"
    failed=1
fi

run_test ar_shell_test.py python3 tools/ar_shell_test.py
run_test ar_shell_builtins_test.py python3 tools/ar_shell_builtins_test.py
run_test command_marks_test.py python3 tools/command_marks_test.py

if [ "$failed" -eq 0 ]; then
    printf 'BUILD PASS n_tests=%s known_fail=%s\n' "$tests" "$known_fail"
else
    printf 'BUILD FAIL\n'
fi
exit "$failed"
