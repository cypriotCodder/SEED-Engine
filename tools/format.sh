#!/bin/sh
# Formats project-owned C++ sources in place. Pass --check to report unformatted files without editing.
set -eu
cd "$(dirname "$0")/.."
seed_format=clang-format
if ! command -v clang-format >/dev/null 2>&1; then
    seed_format=$(xcrun --find clang-format 2>/dev/null || true)
fi
if [ -z "$seed_format" ]; then
    echo 'clang-format not found' >&2
    exit 1
fi
seed_files=$(find src games tests tools -name '*.cpp' -o -name '*.hpp')
if [ "${1:-}" = --check ]; then
    "$seed_format" --dry-run --Werror $seed_files
else
    "$seed_format" -i $seed_files
fi
