core = static_library('pirwa-core',
  ['pirwa-machine.c', 'pirwa-model.c', 'pirwa-inventory.c', 'pirwa-config.c', 'pirwa-provision.c', 'pirwa-backend.c', 'pirwa-boxes.c'],
  dependencies: [glib_dep, gio_dep, gio_unix_dep, libvirt_dep, libvirt_glib_dep, xml_dep, osinfo_dep],
  include_directories: project_includes,
)

core_dep = declare_dependency(
  link_with: core,
  dependencies: [glib_dep, gio_dep, gio_unix_dep, libvirt_dep, libvirt_glib_dep, xml_dep, osinfo_dep],
  include_directories: project_includes,
)

ui = static_library('pirwa-ui', ['pirwa-window.c', 'pirwa-boxes-dialog.c'],
  dependencies: [core_dep, gtk_dep, adwaita_dep],
  include_directories: project_includes,
)
ui_dep = declare_dependency(
  link_with: ui,
  dependencies: [core_dep, gtk_dep, adwaita_dep],
  include_directories: project_includes,
)
executable('pirwa', 'main.c', dependencies: ui_dep, install: true)
