cmake_minimum_required(VERSION 3.14)

project(memory-leak-detector
        VERSION 1.0.0
        DESCRIPTION "Lightweight C++ allocation tracker that reports leaks at exit"
        LANGUAGES CXX)

# ---- Standard & defaults --------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Keep warnings tight; the library overrides global operators, so be deliberate.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---- Threads (the registry uses std::mutex) -------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---- Static library: libmemleak -------------------------------------------
add_library(memleak STATIC
    src/Tracker.cpp
    src/Backtrace.cpp
    src/Overrides.cpp)

target_include_directories(memleak PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_link_libraries(memleak PUBLIC Threads::Threads)

# Some toolchains place backtrace_symbols in a separate library; link it when
# the platform requires it (notably musl-based distributions).
include(CheckSymbolExists)
check_symbol_exists(backtrace "execinfo.h" MEMLEAK_HAVE_BACKTRACE_LIBC)
if(NOT MEMLEAK_HAVE_BACKTRACE_LIBC)
    find_library(EXECINFO_LIBRARY NAMES execinfo)
    if(EXECINFO_LIBRARY)
        target_link_libraries(memleak PUBLIC ${EXECINFO_LIBRARY})
    endif()
endif()

# Exported symbols (backtrace) read better with frame info available.
target_compile_options(memleak PRIVATE
    $<$<CONFIG:Debug>:-g>)

# ---- Demo executable ------------------------------------------------------
add_executable(memleak_demo src/demo.cpp)
target_link_libraries(memleak_demo PRIVATE memleak)

# Preserve symbol names so the demo's reported call sites are meaningful.
if(NOT MSVC)
    target_link_options(memleak_demo PRIVATE -rdynamic)
endif()

# ---- Example workload -----------------------------------------------------
add_executable(track_workload examples/track_workload.cpp)
target_link_libraries(track_workload PRIVATE memleak)
if(NOT MSVC)
    target_link_options(track_workload PRIVATE -rdynamic)
endif()

# ---- Tests ----------------------------------------------------------------
enable_testing()

add_executable(tracker_tests tests/tracker_tests.cpp)
target_link_libraries(tracker_tests PRIVATE memleak)

add_test(NAME tracker_tests COMMAND tracker_tests)
set_tests_properties(tracker_tests PROPERTIES TIMEOUT 60)
