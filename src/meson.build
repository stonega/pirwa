core = static_library('pirwa-core',
  ['pirwa-machine.c', 'pirwa-inventory.c'],
  dependencies: [glib_dep, gio_dep, libvirt_dep],
  include_directories: project_includes,
)

core_dep = declare_dependency(
  link_with: core,
  dependencies: [glib_dep, gio_dep, libvirt_dep],
  include_directories: project_includes,
)

executable('pirwa', ['main.c', 'pirwa-window.c'],
  dependencies: [core_dep, gtk_dep, adwaita_dep],
  include_directories: project_includes,
  install: true,
)
