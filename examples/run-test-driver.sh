#!/bin/sh
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PIRWA_LIBVIRT_URI=test:///default exec "$project_root/build/src/pirwa" "$@"
