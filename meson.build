project(
  'shclog',
  'cpp',
  default_options: [
    'cpp_std=c++26',
    'werror=true',
  ],
)
cc = meson.get_compiler('cpp')

add_project_arguments(
  cc.get_supported_arguments('-Wno-interference-size'),
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  # '-Werror=sign-conversion',
  '-fno-exceptions',
  '-march=native',
  language: 'cpp',
)

shclog_inc = include_directories('include')
shclog_sources = [
  'src' / 'shclog' / 'io' / 'syscall.cpp',
  'src' / 'shclog' / 'io' / 'mmap.cpp',
  'src' / 'shclog' / 'io' / 'file.cpp',
  'src' / 'shclog' / 'io' / 'iouring.cpp',
  'src' / 'shclog' / 'io' / 'cpu.cpp',
  'src' / 'shclog' / 'io' / 'process.cpp',
]
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
test_src = [
  'test' / 'test_main.cpp',
  'test' / 'shclog' / 'test_mpsc_queue.cpp',
  'test' / 'shclog' / 'io' / 'test_mmap.cpp',
  'test' / 'shclog' / 'io' / 'test_cpu.cpp',
  'test' / 'shclog' / 'io' / 'test_iouring.cpp',
]

test_exe = executable(
  'unit_tests',
  test_src,
  objects: shclog_objs,
  include_directories: test_inc,
  dependencies: [doctest_dep],
)

test(
  'unit_tests',
  test_exe,
  workdir: meson.current_source_dir() / 'test',
)

run_target(
  'test-compile',
  command: [
    'sh',
    '-c', 'ninja -C "$1" clang-tidy && meson test -C "$1" --verbose',
    'test-compile',
    meson.project_build_root(),
  ],
  depends: test_exe,
)
