project('mini-dragonfly', 'cpp',
    version : '0.1.0',
    default_options : ['warning_level=3', 'cpp_std=c++23'])

src_inc = include_directories('src')

cmake = import('cmake')

seastar_cxx_flags = ''
if get_option('b_sanitize') != 'none'
    # Any sanitizer's own runtime bootstraps during the dynamic linker's
    # startup, before main() runs, and needs to malloc() while resolving
    # symbols for its own interceptors (e.g. dlsym'ing __isoc99_printf).
    # Seastar globally overrides malloc/operator new with its own
    # allocator; calling into it this early, before Seastar's own
    # allocator state exists, crashes inside the sanitizer's own runtime.
    # This is an init-order race, not deterministic -- it happened not to
    # trigger for a small hello-world binary under ASan, then did trigger
    # once the real (much larger) server binary existed, so don't assume
    # "it worked before" means it's safe for every target.
    # SEASTAR_DEFAULT_ALLOCATOR disables Seastar's custom allocator,
    # falling back to the plain system one, avoiding the clash. Seastar's
    # own CMake only defines this for its own Debug/Sanitize build types,
    # which are tied to ASan+UBSan specifically, so we set it directly
    # instead of routing through that (narrower) machinery.
    seastar_cxx_flags = '-DSEASTAR_DEFAULT_ALLOCATOR'
endif

seastar_opts = cmake.subproject_options()
seastar_opts.add_cmake_defines({
    'CMAKE_BUILD_TYPE' : 'Release',
    'Seastar_TESTING' : 'OFF',
    'Seastar_APPS' : 'OFF',
    'Seastar_DEMOS' : 'OFF',
    # Seastar's own CMake has an ordering bug: cmake/SeastarDependencies.cmake
    # only probes for LibUring if Seastar_IO_URING is already truthy, but
    # CMakeLists.txt only sets Seastar_IO_URING based on whether LibUring was
    # found -- a chicken-and-egg gate that silently resolves to OFF on a
    # clean configure. io_uring works fine on this system (verified directly
    # against liburing), so force it on rather than rely on that detection.
    'Seastar_IO_URING' : 'ON',
    'CMAKE_CXX_FLAGS' : seastar_cxx_flags,
})
seastar_opts.set_install(false)

seastar_proj = cmake.subproject('seastar', options : seastar_opts)
seastar_dep = seastar_proj.dependency('seastar')

mini_dragonfly_exe = executable('mini-dragonfly',
    'src/main.cpp',
    'src/resp.cpp',
    'src/server.cpp',
    'src/store.cpp',
    include_directories : src_inc,
    dependencies : seastar_dep,
    install : true)

catch2_dep = dependency('catch2-with-main')
threads_dep = dependency('threads')

test_exe = executable('mini-dragonfly-tests',
    'tests/smoke_test.cpp',
    'tests/resp_test.cpp',
    'tests/store_test.cpp',
    'src/resp.cpp',
    'src/store.cpp',
    include_directories : src_inc,
    dependencies : catch2_dep)

test('smoke', test_exe)

# Black-box integration tests: spawn the real compiled server as a
# subprocess and drive it over a plain TCP socket, no Seastar needed here.
integration_test_exe = executable('mini-dragonfly-integration-tests',
    'tests/server_test.cpp',
    'tests/tcp_client.cpp',
    'tests/test_server_process.cpp',
    dependencies : [catch2_dep, threads_dep])

test('integration', integration_test_exe,
    env : {'MINI_DRAGONFLY_BIN' : mini_dragonfly_exe.full_path()},
    timeout : 60)

run_target('format',
    command : ['bash', '-c', 'clang-format -i src/*.cpp include/*.hpp include/*.h 2>/dev/null; true'])

run_target('tidy',
    command : ['bash', '-c', 'clang-tidy -p @0@ src/*.cpp'.format(meson.project_build_root())])
