install_data('io.github.stonega.Pirwa.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data('io.github.stonega.Pirwa.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
)
install_data('io.github.stonega.Pirwa-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/symbolic/apps',
)

desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('desktop-file', desktop_validate,
    args: [meson.current_source_dir() / 'io.github.stonega.Pirwa.desktop'],
  )
endif

install_data('io.github.stonega.Pirwa.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

install_data('avatars/README.md',
  install_dir: get_option('datadir') / 'doc/pirwa/avatars',
)
appstream_validate = find_program('appstreamcli', required: false)
if appstream_validate.found()
  test('appstream', appstream_validate,
    args: ['validate', '--strict', '--no-net', meson.current_source_dir() / 'io.github.stonega.Pirwa.metainfo.xml'],
  )
endif

metadata_python = find_program('python3')
test('release-metadata', metadata_python,
  args: [files('../scripts/check-release-metadata.py'), '--app-id', pirwa_application_id,
         '--version', meson.project_version()],
)

settings_schemas = gnome.compile_schemas(depend_files: files('io.github.stonega.Pirwa.gschema.xml'))
install_data('io.github.stonega.Pirwa.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0/schemas',
)
