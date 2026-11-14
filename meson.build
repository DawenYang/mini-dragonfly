project('mini-dragonfly', 'cpp',
  version : '0.1.0',
  default_options : ['warning_level=3', 'cpp_std=c++23'])

executable('mini-dragonfly',
           'src/main.cpp',
           install : true)

catch2_dep = dependency('catch2-with-main')

test_exe = executable('mini-dragonfly-tests',
    'tests/smoke_test.cpp',
    dependencies : catch2_dep)

test('smoke', test_exe)

run_target('format',
    command : ['bash', '-c', 'clang-format -i src/*.cpp include/*.hpp include/*.h 2>/dev/null; true'])

run_target('tidy',
    command : ['bash', '-c', 'clang-tidy -p @0@ src/*.cpp'.format(meson.project_build_root())])
