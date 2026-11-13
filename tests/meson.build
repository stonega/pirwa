inventory_test = executable('test-inventory', 'test-inventory.c', dependencies: core_dep)
test('inventory', inventory_test, timeout: 30)

executable('benchmark-inventory', 'benchmark-inventory.c', dependencies: core_dep)

backend_test = executable('test-backend', 'test-backend.c', dependencies: core_dep)
backend_environment = environment()
backend_environment.prepend('PATH', meson.current_source_dir() / 'fixtures')
test('backend', backend_test, timeout: 45, env: backend_environment)
config_test = executable('test-config', 'test-config.c', dependencies: core_dep)
test('config', config_test)

scale_benchmark = executable('benchmark-scale', 'benchmark-scale.c', dependencies: core_dep)
test('scale-1000', scale_benchmark, args: ['1000'], timeout: 30)

# Never part of the default test suite: requires explicit environment opt-in.
executable('test-kvm', 'test-kvm.c', dependencies: core_dep)

provision_test = executable('test-provision', 'test-provision.c', dependencies: core_dep)
test('provision', provision_test)

# Explicit desktop QA and profiling tool; no display in the default suite.
executable('test-ui', 'test-ui.c', dependencies: ui_dep)

model_test = executable('test-model', 'test-model.c', dependencies: core_dep)
test('model', model_test)

boxes_test = executable('test-boxes', 'test-boxes.c', dependencies: core_dep)
test('boxes', boxes_test, timeout: 30, env: backend_environment)
