cmake_minimum_required(VERSION 3.20)
project(kernel_file_buffers C)
if(NOT WIN32)
    message(FATAL_ERROR "This regression exercises the Windows file backend")
endif()
set(CMAKE_C_STANDARD 11)
set(KERNEL_FILE_TEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/kernel_file.c"
    CACHE FILEPATH "Production file backend, or an older source for a negative control")
add_executable(kernel_file_buffers test_main.c)
target_include_directories(kernel_file_buffers PRIVATE ../../src/kernel ../../include ../../src)
target_compile_definitions(kernel_file_buffers PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    KERNEL_FILE_TEST_SOURCE="${KERNEL_FILE_TEST_SOURCE}")
enable_testing()
add_test(NAME kernel-file-buffers COMMAND kernel_file_buffers)
