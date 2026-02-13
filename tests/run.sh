#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BINARY="$REPO_ROOT/bin/bbr"
CASES_DIR="$SCRIPT_DIR/cases"

if [ ! -x "$BINARY" ]; then
    echo "error: missing executable '$BINARY' (run 'make bbr')" >&2
    exit 1
fi

if [ ! -d "$CASES_DIR" ]; then
    echo "error: missing cases directory '$CASES_DIR'" >&2
    exit 1
fi

case_files=$(find "$CASES_DIR" -maxdepth 1 -type f -name '*.in' | sort)
if [ -z "$case_files" ]; then
    echo "error: no test case found in '$CASES_DIR'" >&2
    exit 1
fi

cleanup_files=""
cleanup() {
    if [ -n "$cleanup_files" ]; then
        # shellcheck disable=SC2086
        rm -f $cleanup_files
    fi
}
trap cleanup EXIT HUP INT TERM

passed=0
failed=0
total=0

for case_in in $case_files; do
    total=$((total + 1))

    base=${case_in%.in}
    case_name=$(basename "$base")
    expected_out="$base.out"
    expected_code_file="$base.code"

    if [ ! -f "$expected_out" ]; then
        echo "not ok $case_name"
        echo "  missing expected output file: $expected_out"
        failed=$((failed + 1))
        continue
    fi

    actual_out=$(mktemp "${TMPDIR:-/tmp}/bbr-test-out.XXXXXX")
    cleanup_files="$cleanup_files $actual_out"

    if "$BINARY" < "$case_in" > "$actual_out"; then
        actual_code=0
    else
        actual_code=$?
    fi

    if [ -f "$expected_code_file" ]; then
        expected_code=$(tr -d ' \t\r\n' < "$expected_code_file")
        case "$expected_code" in
            ''|*[!0-9]*)
                echo "not ok $case_name"
                echo "  invalid expected exit code in $expected_code_file"
                failed=$((failed + 1))
                continue
                ;;
        esac
    else
        expected_code=0
    fi

    case_ok=1
    reported_failure=0

    if [ "$actual_code" -ne "$expected_code" ]; then
        case_ok=0
        if [ "$reported_failure" -eq 0 ]; then
            echo "not ok $case_name"
            reported_failure=1
        fi
        echo "  exit code mismatch: expected $expected_code, got $actual_code"
    fi

    if ! cmp -s "$expected_out" "$actual_out"; then
        case_ok=0
        if [ "$reported_failure" -eq 0 ]; then
            echo "not ok $case_name"
            reported_failure=1
        fi
        echo "  stdout mismatch"
        echo "  diff (- expected, + actual):"
        diff -u "$expected_out" "$actual_out" || true
        echo "  expected bytes (hex):"
        od -An -t x1 "$expected_out"
        echo "  actual bytes (hex):"
        od -An -t x1 "$actual_out"
    fi

    if [ "$case_ok" -eq 1 ]; then
        echo "ok $case_name"
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
    fi
done

echo "summary: $passed/$total passed, $failed failed"

if [ "$failed" -ne 0 ]; then
    exit 1
fi
