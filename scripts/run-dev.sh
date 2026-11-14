#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

# Prefer the installed viewer; use this host's temporary dependency prefix
# when it is still available, for the VNC/SPICE GL fallback. SPICE runs inside Pirwa.
if ! command -v virt-viewer >/dev/null 2>&1 && [ -r /tmp/pirwa-build-deps-path ]; then
  pirwa_dependencies=$(cat /tmp/pirwa-build-deps-path)
  if [ -x "$pirwa_dependencies/usr/bin/virt-viewer" ]; then
    PATH="$pirwa_dependencies/usr/bin:$PATH"
    export PATH
  fi
fi

meson compile -C build
exec "$project_root/build/src/pirwa" "$@"
