install_data('io.github.pirwa.Pirwa.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data('io.github.pirwa.Pirwa.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps',
)

desktop_validate = find_program('desktop-file-validate', required: false)
if desktop_validate.found()
  test('desktop-file', desktop_validate,
    args: [meson.current_source_dir() / 'io.github.pirwa.Pirwa.desktop'],
  )
endif

install_data('io.github.pirwa.Pirwa.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)
appstream_validate = find_program('appstreamcli', required: false)
if appstream_validate.found()
  test('appstream', appstream_validate,
    # The development project has no published homepage yet. Validate all other tags normally.
    args: ['validate', '--no-net', '--override', 'url-homepage-missing=info', meson.current_source_dir() / 'io.github.pirwa.Pirwa.metainfo.xml'],
  )
endif
