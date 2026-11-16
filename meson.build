project('mini-dragonfly', 'cpp',
    version : '0.1.0',
    default_options : ['warning_level=3', 'cpp_std=c++23'])

src_inc = include_directories('src')

cmake = import('cmake')

seastar_opts = cmake.subproject_options()
seastar_opts.add_cmake_defines({
    'CMAKE_BUILD_TYPE' : 'Release',
    'Seastar_TESTING' : 'OFF',
    'Seastar_APPS' : 'OFF',
    'Seastar_DEMOS' : 'OFF',
})
seastar_opts.set_install(false)

seastar_proj = cmake.subproject('seastar', options : seastar_opts)
seastar_dep = seastar_proj.dependency('seastar')

executable('mini-dragonfly',
    'src/main.cpp',
    'src/resp.cpp',
    include_directories : src_inc,
    install : true)

catch2_dep = dependency('catch2-with-main')

test_exe = executable('mini-dragonfly-tests',
    'tests/smoke_test.cpp',
    'tests/resp_test.cpp',
    'src/resp.cpp',
    include_directories : src_inc,
    dependencies : catch2_dep)

test('smoke', test_exe)

run_target('format',
    command : ['bash', '-c', 'clang-format -i src/*.cpp include/*.hpp include/*.h 2>/dev/null; true'])

run_target('tidy',
    command : ['bash', '-c', 'clang-tidy -p @0@ src/*.cpp'.format(meson.project_build_root())])
