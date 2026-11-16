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

if [ ! -f build/meson-private/coredata.dat ]; then
  meson setup build
fi

if ! meson compile -C build; then
  cat >&2 <<'EOF'

Pirwa could not be rebuilt. Check the compiler error above and the development
dependencies in docs/implementation/setup.md.

If a previous build used SDK headers under /tmp, restore the development
dependencies and clear Meson's cached paths before trying again:

  meson setup build --reconfigure --clearcache '-Dpkg_config_path=[]'

For a local SDK, replace [] with its persistent pkg-config directories.
EOF
  exit 1
fi
exec "$project_root/build/src/pirwa" "$@"
