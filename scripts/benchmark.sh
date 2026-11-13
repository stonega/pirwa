#!/bin/sh
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"
build_directory=${1:-build}
meson compile -C "$build_directory" benchmark-inventory
"$build_directory/tests/benchmark-inventory"
