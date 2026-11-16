Name:           pirwa
Version:        0.1.1
Release:        1%{?dist}
%{!?pirwa_source_version:%global pirwa_source_version %{version}}
Summary:        Native GNOME virtual machine manager
License:        GPL-3.0-or-later
URL:            https://github.com/stonega/pirwa
Source0:        %{name}-%{pirwa_source_version}.tar.xz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  python3
BuildRequires:  git
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(gtk4) >= 4.16
BuildRequires:  pkgconfig(spice-client-glib-2.0) >= 0.42
BuildRequires:  pkgconfig(libadwaita-1)
BuildRequires:  pkgconfig(libvirt)
BuildRequires:  pkgconfig(libvirt-glib-1.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(libosinfo-1.0)
BuildRequires:  pkgconfig(slirp) >= 4.7
Recommends:     virt-viewer
Recommends:     edk2-ovmf
Recommends:     swtpm
Recommends:     swtpm-tools
Requires:       qemu-kvm
Requires:       libvirt-daemon-driver-qemu
Requires:       libvirt-daemon-driver-storage-core
Requires:       osinfo-db
Requires:       gsettings-desktop-schemas

%description
Pirwa manages local libvirt virtual machines with a native GTK 4 and libadwaita
interface and a native SPICE console. VNC and SPICE GL consoles open in virt-viewer.

%prep
%autosetup -n %{name}-%{pirwa_source_version}

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.stonega.Pirwa.desktop
appstreamcli validate-tree --strict --no-net %{buildroot}%{_prefix}

%files
%license COPYING
%doc README.md
%{_datadir}/doc/pirwa/avatars/
%{_bindir}/pirwa
%{_libexecdir}/pirwa-network
%{_datadir}/applications/io.github.stonega.Pirwa.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.stonega.Pirwa.svg
%{_datadir}/icons/hicolor/symbolic/apps/io.github.stonega.Pirwa-symbolic.svg
%{_datadir}/metainfo/io.github.stonega.Pirwa.metainfo.xml
%{_datadir}/glib-2.0/schemas/io.github.stonega.Pirwa.gschema.xml

%changelog
* Mon Nov 16 2026 Pirwa Contributors - 0.1.1-1
- Restore the machine display after returning to the application.
- Recover automatically when an established SPICE display surface disappears.

* Mon Nov 16 2026 Pirwa Contributors - 0.1.0-1
- First public release with native and Flatpak distribution.

* Fri Nov 13 2026 Pirwa Contributors - 0.1.0-0.1.dev
- Initial development package.
