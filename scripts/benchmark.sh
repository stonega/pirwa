#!/bin/sh
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
build_directory=${1:-build}
meson compile -C "$build_directory" benchmark-inventory benchmark-scale
"$build_directory/tests/benchmark-inventory"
for count in 0 100 1000; do
  "$build_directory/tests/benchmark-scale" "$count"
done
