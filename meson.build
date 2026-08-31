project(
  'shclog',
  'cpp',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'werror=true',
    'cpp_args=-fno-exceptions -Wno-interference-size',
  ],
)

shclog_inc = include_directories('include')
shclog_sources = []
shclog_lib = static_library('shclog_lib', shclog_sources, include_directories: shclog_inc)

# executable(
#     'shclog_app',
#     'apps/main.cpp',
#     include_directories: shclog_inc,
#     link_with: shclog_lib,
# )

## Tests
doctest_dep = dependency('doctest', fallback: ['doctest'])

shclog_objs = shclog_lib.extract_all_objects(recursive: true)
test_inc = include_directories('test', 'include')
test_src = ['test' / 'shclog' / 'test_mpsc_queue.cpp']

test_exe = executable(
  'unit_tests',
  test_src,
  objects: shclog_objs,
  include_directories: test_inc,
  dependencies: [doctest_dep],
)
test('unit_tests', test_exe)

run_target('test-compile', command: ['meson', 'test', '--verbose'], depends: test_exe)
