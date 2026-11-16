#!/bin/sh
# Run inside the rootless QA image, with /src frozen/read-only and /out writable.
set -eu
cp -a /src/. /work/
cd /work
printf '%s\n' configure > /out/stage
meson setup build --prefix=/usr --buildtype=release
meson compile -C build -j 2
printf '%s\n' headless > /out/stage
meson test -C build --no-rebuild --print-errorlogs
printf '%s\n' format > /out/stage
clang-format --dry-run --Werror src/*.[ch] tests/*.c
printf '%s\n' sanitize > /out/stage
meson setup build-sanitize --prefix=/usr -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-sanitize -j 2
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS="suppressions=$PWD/tests/lsan.supp:print_suppressions=1" \
    meson test -C build-sanitize --no-rebuild --print-errorlogs
printf '%s\n' staged-install > /out/stage
DESTDIR=/tmp/pirwa-stage meson install -C build
glib-compile-schemas --strict --dry-run /tmp/pirwa-stage/usr/share/glib-2.0/schemas
appstreamcli validate-tree --strict --no-net /tmp/pirwa-stage/usr > /out/staged-appstream.txt
/tmp/pirwa-stage/usr/bin/pirwa --version > /out/staged-version.txt
mkdir -p /out/normal/src /out/normal/tests /out/normal/data /out/sanitizer/src /out/sanitizer/tests /out/sanitizer/data
cp build/src/pirwa /out/normal/src/
cp build-sanitize/src/pirwa /out/sanitizer/src/
cp build/data/gschemas.compiled /out/normal/data/
cp build-sanitize/data/gschemas.compiled /out/sanitizer/data/
for executable in test-backend-daemon test-ui test-console-ui test-ui-lifetime; do
    cp "build/tests/$executable" /out/normal/tests/
    cp "build-sanitize/tests/$executable" /out/sanitizer/tests/
done
cp build/meson-logs/testlog.json /out/normal/
cp build-sanitize/meson-logs/testlog.json /out/sanitizer/
rpm -qa | sort > /out/packages.txt
if [ "${1:-}" = '--rpm' ]; then
    printf '%s\n' rpm-build > /out/stage
    mkdir -p /tmp/rpmbuild/SOURCES /out/rpms
    pirwa_version=$(meson introspect --projectinfo build | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
    case "$pirwa_version" in
        ''|*[!a-zA-Z0-9.-]*) printf '%s\n' 'Invalid source version' >&2; exit 1 ;;
    esac
    tar -cJf "/tmp/rpmbuild/SOURCES/pirwa-$pirwa_version.tar.xz" \
        --transform="s,^,pirwa-$pirwa_version/," -C /src .
    rpmbuild --define '_topdir /tmp/rpmbuild' --define "pirwa_source_version $pirwa_version" --define '_smp_mflags -j2' \
        --define '_smp_build_ncpus 2' -bb packaging/pirwa.spec
    cp /tmp/rpmbuild/RPMS/*/*.rpm /out/rpms/
    printf '%s\n' rpm-install > /out/stage
    dnf install -y --disablerepo='*' --setopt=install_weak_deps=False /out/rpms/pirwa-[0-9]*.rpm
    rpm -V pirwa
    /usr/bin/pirwa --version > /out/installed-version.txt
fi
printf '%s\n' complete > /out/stage
