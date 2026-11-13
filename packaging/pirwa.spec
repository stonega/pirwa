Name:           pirwa
Version:        0.1.0
Release:        0.1.dev%{?dist}
Summary:        Native GNOME virtual machine manager
License:        GPL-3.0-or-later
Source0:        %{name}-%{version}-dev.tar.xz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libadwaita-1)
BuildRequires:  pkgconfig(libvirt)
BuildRequires:  pkgconfig(libvirt-glib-1.0)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(libosinfo-1.0)
Requires:       virt-viewer
Requires:       qemu-kvm
Requires:       libvirt-daemon-driver-qemu
Requires:       osinfo-db

%description
Pirwa manages local libvirt virtual machines with a native GTK 4 and libadwaita
interface. Graphical consoles open in virt-viewer.

%prep
%autosetup -n %{name}-%{version}-dev

%build
%meson
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.pirwa.Pirwa.desktop

%files
%license COPYING
%doc README.md
%{_bindir}/pirwa
%{_datadir}/applications/io.github.pirwa.Pirwa.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.pirwa.Pirwa.svg
%{_datadir}/metainfo/io.github.pirwa.Pirwa.metainfo.xml

%changelog
* Fri Nov 13 2026 Pirwa Contributors - 0.1.0-0.1.dev
- Initial development package.
