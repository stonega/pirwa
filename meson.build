project('pirwa', 'c',
  version: '0.1.3',
  meson_version: '>= 1.3.0',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c17', 'warning_level=3', 'werror=true', 'buildtype=debugoptimized'],
)

gnome = import('gnome')
glib_dep = dependency('glib-2.0', version: '>= 2.76')
gio_dep = dependency('gio-2.0', version: '>= 2.76')
gio_unix_dep = dependency('gio-unix-2.0', version: '>= 2.76')
gtk_dep = dependency('gtk4', version: '>= 4.16')
spice_dep = dependency('spice-client-glib-2.0', version: '>= 0.42', include_type: 'system')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.5')
libvirt_dep = dependency('libvirt', version: '>= 9.0', include_type: 'system')
libvirt_glib_dep = dependency('libvirt-glib-1.0', version: '>= 4.0', include_type: 'system')
xml_dep = dependency('libxml-2.0', version: '>= 2.9')
osinfo_dep = dependency('libosinfo-1.0', version: '>= 1.10', include_type: 'system')
slirp_dep = dependency('slirp', version: '>= 4.7', include_type: 'system')

config = configuration_data()
config.set_quoted('PIRWA_VERSION', meson.project_version())
config.set_quoted('GETTEXT_PACKAGE', 'pirwa')
config.set_quoted('PIRWA_SCHEMA_DIR', meson.current_build_dir() / 'data')
config.set_quoted('PIRWA_NETWORK_BUILD', meson.current_build_dir() / 'src/pirwa-network')
config.set_quoted('PIRWA_NETWORK_INSTALLED', get_option('prefix') / get_option('libexecdir') / 'pirwa-network')
config.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
# Application identity follows the repository owner's GitHub namespace.
pirwa_application_id = 'io.github.stonega.Pirwa'
config.set_quoted('PIRWA_APPLICATION_ID', pirwa_application_id)
configure_file(output: 'config.h', configuration: config)
project_includes = include_directories('.', 'src')

subdir('src')
subdir('data')
subdir('tests')
subdir('po')

gnome.post_install(glib_compile_schemas: true, gtk_update_icon_cache: true, update_desktop_database: true)
