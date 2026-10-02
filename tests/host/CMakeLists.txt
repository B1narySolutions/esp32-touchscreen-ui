# Host unit tests for the Seed link protocol (no ESP-IDF). Run with tools/run_host_tests.ps1, or:
#   cmake -S tests/host -B build/host -G Ninja && cmake --build build/host && build/host/host_tests
cmake_minimum_required(VERSION 3.16)
project(seed_link_host_tests C)

set(PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware/main/seed_link_proto")
set(PROTO_SRC "${PROTO_DIR}/seed_link_proto.c" "${PROTO_DIR}/slp_coalesce.c" "${PROTO_DIR}/slp_receiver.c")
set(NAM_DIR "${CMAKE_CURRENT_LIST_DIR}/../../firmware/main/nam")
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(MSVC)
    set(WARN /W4 /WX /wd4204) # 4204: non-constant aggregate initializer (valid C99)
    set(SAN /fsanitize=address /Zi)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(WARN -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion)
    set(SAN -fsanitize=address,undefined -fno-omit-frame-pointer -g)
endif()

add_library(slp STATIC ${PROTO_SRC} "${NAM_DIR}/nam_a2.c")
target_include_directories(slp PUBLIC "${PROTO_DIR}" "${NAM_DIR}")
target_compile_options(slp PRIVATE ${WARN} ${SAN})
set_property(TARGET slp PROPERTY C_STANDARD 99)

add_executable(host_tests main.c test_proto.c test_fuzz.c test_coalesce.c test_nam.c test_receiver.c)
target_compile_definitions(host_tests PRIVATE REPO_ROOT="${REPO_ROOT}")
target_link_libraries(host_tests PRIVATE slp)
target_compile_options(host_tests PRIVATE ${WARN} ${SAN})
set_property(TARGET host_tests PROPERTY C_STANDARD 11)

# Runs the .nam loader on one file (tools/nam_diff_test.py compares it with convert_a2.py).
add_executable(nam_check nam_check.c)
target_link_libraries(nam_check PRIVATE slp)
target_compile_options(nam_check PRIVATE ${WARN} ${SAN})
set_property(TARGET nam_check PROPERTY C_STANDARD 11)

# Runs the Seed-side receiver on a PC serial port against the real ESP32 (Windows).
add_executable(seed_sim seed_sim.c)
target_link_libraries(seed_sim PRIVATE slp)
target_compile_options(seed_sim PRIVATE ${WARN})
set_property(TARGET seed_sim PROPERTY C_STANDARD 11)

# Prints the hex examples used in docs/SEED_LINK_PROTOCOL.md.
add_executable(slp_examples examples.c)
target_link_libraries(slp_examples PRIVATE slp)
target_compile_options(slp_examples PRIVATE ${WARN} ${SAN})
set_property(TARGET slp_examples PROPERTY C_STANDARD 11)

if(NOT MSVC)
    target_link_options(host_tests PRIVATE ${SAN})
    target_link_options(slp_examples PRIVATE ${SAN})
    target_link_options(nam_check PRIVATE ${SAN})
else()
    # The ASan runtime DLL is only on PATH inside a vcvars shell; put a copy next to the
    # executables so they also run from a plain prompt (tools/test_slp_py.py runs slp_examples).
    get_filename_component(cl_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    set(asan_dll "${cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${asan_dll}")
        add_custom_command(TARGET host_tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${asan_dll}" "$<TARGET_FILE_DIR:host_tests>")
    endif()
endif()

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
