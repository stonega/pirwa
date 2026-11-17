inventory_test = executable('test-inventory', 'test-inventory.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virDomainGetMetadata'])
test('inventory', inventory_test, timeout: 30)

executable('benchmark-inventory', 'benchmark-inventory.c', dependencies: core_dep)

backend_test = executable('test-backend', 'test-backend.c', 'qa-media-driver.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virDomainUpdateDeviceFlags'])
backend_environment = environment()
backend_environment.prepend('PATH', meson.current_source_dir() / 'fixtures')
test('backend', backend_test, timeout: 45, env: backend_environment)

# Test-driver state with injected transport/permission failures; no daemon or VM.
backend_fault_test = executable('test-backend-faults', 'test-backend-faults.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virConnectOpen', '-Wl,--wrap=virConnectClose',
    '-Wl,--wrap=virConnectRegisterCloseCallback', '-Wl,--wrap=virConnectUnregisterCloseCallback',
    '-Wl,--wrap=virConnectIsAlive', '-Wl,--wrap=virConnectGetType',
    '-Wl,--wrap=virDomainCreate', '-Wl,--wrap=virDomainFree',
    '-Wl,--wrap=virGetLastError', '-Wl,--wrap=virConnectGetAllDomainStats',
    '-Wl,--wrap=g_async_queue_pop', '-Wl,--wrap=g_async_queue_timeout_pop',
    '-Wl,--wrap=g_thread_new'])
test('backend-faults', backend_fault_test, timeout: 30)
backend_lifetime_test = executable('test-backend-lifetime', 'test-backend-lifetime.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=g_thread_new', '-Wl,--wrap=g_async_queue_new_full',
    '-Wl,--wrap=g_async_queue_unref'])
test('backend-lifetime', backend_lifetime_test, timeout: 30)
usage_test = executable('test-usage', 'test-usage.c', dependencies: core_dep)
test('usage', usage_test)
config_test = executable('test-config', 'test-config.c', dependencies: core_dep)
test('config', config_test)

scale_benchmark = executable('benchmark-scale', 'benchmark-scale.c', dependencies: core_dep)
test('scale-1000', scale_benchmark, args: ['1000'], timeout: 30)

# Never part of the default test suite: requires explicit environment opt-in.
executable('test-kvm', 'test-kvm.c', dependencies: core_dep)
executable('qa-vm', 'qa-vm.c', dependencies: core_dep)

# Explicit private-daemon QA; no default test entry and no guest operations.
executable('test-backend-daemon', 'test-backend-daemon.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=g_thread_new'])

provision_test = executable('test-provision', 'test-provision.c', dependencies: core_dep)
test('provision', provision_test)

# Real test-driver provisioning with synthetic ISO and host capabilities.
windows_test = executable('test-windows', 'test-windows.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virConnectGetDomainCapabilities',
    '-Wl,--wrap=osinfo_media_create_from_location_with_flags',
    '-Wl,--wrap=osinfo_db_identify_media', '-Wl,--wrap=osinfo_media_get_os',
    '-Wl,--wrap=virStorageVolCreateXML', '-Wl,--wrap=virStorageVolDelete',
    '-Wl,--wrap=virDomainDefineXML', '-Wl,--wrap=virDomainUndefineFlags',
    '-Wl,--wrap=pirwa_network_start'])
test('windows-provision', windows_test)

# Explicit desktop QA and profiling tool; no display in the default suite.
executable('test-ui', 'test-ui.c', 'qa-frame-feedback.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=pirwa_boxes_scan_finish'])
executable('test-create-ui', 'test-create-ui.c', dependencies: ui_dep)
executable('test-avatar-ui', 'test-avatar-ui.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=pirwa_backend_request_async'])

# Explicit desktop ownership regression; uses only private test-driver fixtures.
executable('test-ui-lifetime', 'test-ui-lifetime.c', dependencies: ui_dep,
  link_depends: settings_schemas,
  link_args: ['-Wl,--wrap=g_object_new', '-Wl,--wrap=g_thread_new',
    '-Wl,--wrap=g_settings_new_full', '-Wl,--wrap=spice_session_new', '-Wl,--wrap=spice_audio_get',
    '-Wl,--wrap=pirwa_backend_request_finish'])

# Optional startup observer for the production executable; never installed.
if host_machine.system() == 'linux'
  profile_cc = meson.get_compiler('c')
  profile_dl = profile_cc.find_library('dl', required: false)
  shared_module('qa-application-profile', 'qa-application-profile.c', 'qa-frame-feedback.c',
    name_prefix: '', dependencies: [gtk_dep, profile_dl], include_directories: project_includes)
endif

model_test = executable('test-model', 'test-model.c', dependencies: core_dep)
test('model', model_test)

boxes_test = executable('test-boxes', 'test-boxes.c', dependencies: core_dep)
test('boxes', boxes_test, timeout: 30, env: backend_environment)

frame_test = executable('test-frame', 'test-frame.c', '../src/pirwa-frame.c',
  dependencies: [gtk_dep, spice_dep], include_directories: project_includes)
test('console-frame', frame_test)

# Headless creation/copy timing, separate from GUI and guest measurements.
executable('benchmark-frame', 'benchmark-frame.c', '../src/pirwa-frame.c',
  dependencies: [gtk_dep, spice_dep], include_directories: project_includes)

# Explicit frame profiling/GUI correctness; no libvirt connection or VM required.
executable('benchmark-display', 'benchmark-display.c', 'qa-frame-feedback.c', '../src/pirwa-display.c', '../src/pirwa-frame.c',
  dependencies: [gtk_dep, spice_dep], include_directories: project_includes)

# Linux/glibc lifecycle diagnostics; the app has no tracking or sampling hooks.
diagnostic_cc = meson.get_compiler('c')
if host_machine.system() == 'linux' and diagnostic_cc.has_function('mallinfo2', prefix: '#include <malloc.h>') and diagnostic_cc.has_function('malloc_info', prefix: '#include <malloc.h>') and diagnostic_cc.has_function('malloc_trim', prefix: '#include <malloc.h>')
  executable('benchmark-display-memory', 'benchmark-display-memory.c',
    '../src/pirwa-display.c', '../src/pirwa-frame.c',
    dependencies: [gtk_dep, spice_dep], include_directories: project_includes,
    link_args: ['-Wl,--wrap=pirwa_frame_texture', '-Wl,--wrap=pirwa_frame_texture_rect'])
endif

# Explicit embedded/detached viewer lifecycle QA with synthetic domains only.
executable('test-console-ui', 'test-console-ui.c', 'qa-media-driver.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=virDomainUpdateDeviceFlags', '-Wl,--wrap=pirwa_backend_request_async'])

# Explicit desktop error-layout QA; test driver only, with a rejected proxy start.
executable('test-network-ui', 'test-network-ui.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=pirwa_network_start'])

# Explicit desktop regression; accelerated timers and synthetic SPICE frames only.
executable('test-console-connect', 'test-console-connect.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=spice_session_open_fd', '-Wl,--wrap=spice_channel_connect',
    '-Wl,--wrap=pirwa_backend_request_finish', '-Wl,--wrap=g_timeout_add_seconds',
    '-Wl,--wrap=spice_main_channel_update_display_enabled',
    '-Wl,--wrap=spice_main_channel_update_display',
    '-Wl,--wrap=spice_inputs_channel_position', '-Wl,--wrap=pirwa_display_set_primary',
    '-Wl,--wrap=gtk_window_is_active', '-Wl,--wrap=gtk_window_is_suspended'])

# Explicit real-guest profiler. Wrappers and input controls never enter Pirwa.
executable('benchmark-console', 'benchmark-console.c', 'qa-frame-feedback.c', dependencies: ui_dep,
  link_args: ['-Wl,--wrap=spice_session_new', '-Wl,--wrap=pirwa_display_set_primary',
    '-Wl,--wrap=pirwa_display_invalidate', '-Wl,--wrap=pirwa_frame_texture',
    '-Wl,--wrap=pirwa_frame_texture_rect', '-Wl,--wrap=gtk_snapshot_append_texture'])

# The synthetic libvirt driver has no stream transport. Wrap only its upload
# and on-disk deletion; retain real test-driver domain/storage operations.
import_test = executable('test-import', 'test-import.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virConnectOpenReadOnly', '-Wl,--wrap=virStreamNew', '-Wl,--wrap=virStorageVolUpload',
    '-Wl,--wrap=virStreamSend', '-Wl,--wrap=virStreamFinish', '-Wl,--wrap=virStreamAbort',
    '-Wl,--wrap=virStreamFree', '-Wl,--wrap=virStorageVolDelete'])
test('boxes-import', import_test, timeout: 30)

subdir('fixtures')
preferences_test = executable('test-preferences', 'test-preferences.c', '../src/pirwa-preferences.c',
  dependencies: gio_dep, include_directories: project_includes)
test('preferences', preferences_test, depends: [settings_schemas, proxy_test_schemas],
  env: ['GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir() / 'fixtures'])

proxy_test = executable('test-proxy', 'test-proxy.c', dependencies: core_dep)
test('proxy-settings', proxy_test, depends: proxy_test_schemas,
  env: ['GSETTINGS_BACKEND=memory', 'GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir() / 'fixtures'])
network_test = executable('test-network', 'test-network.c', dependencies: core_dep)
test('network-definition', network_test)
network_start_test = executable('test-network-start', 'test-network-start.c', dependencies: core_dep,
  link_args: ['-Wl,--wrap=virConnectGetURI', '-Wl,--wrap=virDomainCreateXML',
    '-Wl,--wrap=virDomainCreate', '-Wl,--wrap=g_subprocess_launcher_spawn'])
test('network-start', network_start_test, timeout: 20,
  depends: [network_helper, settings_schemas, proxy_test_schemas],
  env: ['GSETTINGS_BACKEND=memory', 'GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir() / 'fixtures'])
python = find_program('python3')
test('ui-profile-guards', python, args: [files('test-ui-profile.py')])
test('release-guards', python, args: [files('test-release.py')])
test('network-gateway', python,
  args: [files('test-network-gateway.py'), network_helper], timeout: 45,
  depends: network_helper)
