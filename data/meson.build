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
