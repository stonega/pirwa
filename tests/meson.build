inventory_test = executable('test-inventory', 'test-inventory.c', dependencies: core_dep)
test('inventory', inventory_test, timeout: 30)

executable('benchmark-inventory', 'benchmark-inventory.c', dependencies: core_dep)

backend_test = executable('test-backend', 'test-backend.c', dependencies: core_dep)
backend_environment = environment()
backend_environment.prepend('PATH', meson.current_source_dir() / 'fixtures')
test('backend', backend_test, timeout: 45, env: backend_environment)
usage_test = executable('test-usage', 'test-usage.c', dependencies: core_dep)
test('usage', usage_test)
config_test = executable('test-config', 'test-config.c', dependencies: core_dep)
test('config', config_test)

scale_benchmark = executable('benchmark-scale', 'benchmark-scale.c', dependencies: core_dep)
test('scale-1000', scale_benchmark, args: ['1000'], timeout: 30)

# Never part of the default test suite: requires explicit environment opt-in.
executable('test-kvm', 'test-kvm.c', dependencies: core_dep)
executable('qa-vm', 'qa-vm.c', dependencies: core_dep)

provision_test = executable('test-provision', 'test-provision.c', dependencies: core_dep)
test('provision', provision_test)

# Explicit desktop QA and profiling tool; no display in the default suite.
executable('test-ui', 'test-ui.c', dependencies: ui_dep)

model_test = executable('test-model', 'test-model.c', dependencies: core_dep)
test('model', model_test)

boxes_test = executable('test-boxes', 'test-boxes.c', dependencies: core_dep)
test('boxes', boxes_test, timeout: 30, env: backend_environment)

frame_test = executable('test-frame', 'test-frame.c', '../src/pirwa-frame.c',
  dependencies: [gtk_dep, spice_dep], include_directories: project_includes)
test('console-frame', frame_test)

# Explicit frame profiling/GUI correctness; no libvirt connection or VM required.
executable('benchmark-display', 'benchmark-display.c', '../src/pirwa-display.c', '../src/pirwa-frame.c',
  dependencies: [gtk_dep, spice_dep], include_directories: project_includes)

# Explicit embedded/detached viewer lifecycle QA with synthetic domains only.
executable('test-console-ui', 'test-console-ui.c', dependencies: ui_dep)

# Explicit real-guest profiler. Wrappers and input controls never enter Pirwa.
executable('benchmark-console', 'benchmark-console.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=spice_session_new', '-Wl,--wrap=pirwa_display_set_primary',
    '-Wl,--wrap=pirwa_display_invalidate', '-Wl,--wrap=pirwa_frame_texture',
    '-Wl,--wrap=pirwa_frame_texture_rect'])

# The synthetic libvirt driver has no stream transport. Wrap only its upload
# and on-disk deletion; retain real test-driver domain/storage operations.
import_test = executable('test-import', 'test-import.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virConnectOpenReadOnly', '-Wl,--wrap=virStreamNew', '-Wl,--wrap=virStorageVolUpload',
    '-Wl,--wrap=virStreamSend', '-Wl,--wrap=virStreamFinish', '-Wl,--wrap=virStreamAbort',
    '-Wl,--wrap=virStreamFree', '-Wl,--wrap=virStorageVolDelete'])
test('boxes-import', import_test, timeout: 30)
