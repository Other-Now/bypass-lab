cmake_minimum_required(VERSION 3.20)
project(bypass_lab CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "bypass-lab is Linux-only (sockets/AF_XDP/DPDK). Use WSL2 or a Linux VM.")
endif()

set(BL_WITH_XDP AUTO CACHE STRING "AF_XDP path: AUTO|ON|OFF (needs libxdp, libbpf, clang)")
set(BL_WITH_DPDK AUTO CACHE STRING "DPDK path: AUTO|ON|OFF (needs libdpdk)")
option(BL_SANITIZE "Build with ASan+UBSan" OFF)

set(BL_WARN -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
set(BL_OPT -O2 -march=x86-64-v3)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(BL_OPT -O2)
endif()
if(BL_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig)

add_library(bl_core INTERFACE)
target_include_directories(bl_core INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(bl_core INTERFACE ${BL_WARN} ${BL_OPT})

# --- AF_XDP ----------------------------------------------------------------
set(BL_HAVE_XDP OFF)
if(NOT BL_WITH_XDP STREQUAL "OFF" AND PkgConfig_FOUND)
  pkg_check_modules(LIBXDP IMPORTED_TARGET libxdp)
  pkg_check_modules(LIBBPF IMPORTED_TARGET libbpf)
  find_program(BL_CLANG NAMES clang clang-18 clang-17 clang-16 clang-15 clang-14)
  if(LIBXDP_FOUND AND LIBBPF_FOUND AND BL_CLANG)
    set(BL_HAVE_XDP ON)
  endif()
endif()
if(BL_WITH_XDP STREQUAL "ON" AND NOT BL_HAVE_XDP)
  message(FATAL_ERROR "BL_WITH_XDP=ON but libxdp/libbpf/clang not found")
endif()

# --- DPDK ------------------------------------------------------------------
set(BL_HAVE_DPDK OFF)
if(NOT BL_WITH_DPDK STREQUAL "OFF" AND PkgConfig_FOUND)
  pkg_check_modules(LIBDPDK IMPORTED_TARGET libdpdk)
  if(LIBDPDK_FOUND)
    set(BL_HAVE_DPDK ON)
  endif()
endif()
if(BL_WITH_DPDK STREQUAL "ON" AND NOT BL_HAVE_DPDK)
  message(FATAL_ERROR "BL_WITH_DPDK=ON but libdpdk not found")
endif()

message(STATUS "bypass-lab: AF_XDP=${BL_HAVE_XDP} DPDK=${BL_HAVE_DPDK}")

# --- BPF object ------------------------------------------------------------
set(BL_BPF_OBJ ${CMAKE_BINARY_DIR}/xdp_udp_redirect.bpf.o)
if(BL_HAVE_XDP)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-multiarch
                  OUTPUT_VARIABLE BL_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_custom_command(
    OUTPUT ${BL_BPF_OBJ}
    COMMAND ${BL_CLANG} -O2 -g -target bpf -Wall
            -I/usr/include/${BL_MULTIARCH}
            -c ${CMAKE_SOURCE_DIR}/src/bpf/xdp_udp_redirect.bpf.c -o ${BL_BPF_OBJ}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/bpf/xdp_udp_redirect.bpf.c
    COMMENT "Compiling XDP program")
  add_custom_target(bl_bpf ALL DEPENDS ${BL_BPF_OBJ})
endif()

# --- binaries --------------------------------------------------------------
add_executable(bl_sender src/sender.cpp)
target_link_libraries(bl_sender PRIVATE bl_core Threads::Threads)

add_executable(bl_rx src/rx_main.cpp src/rx_socket.cpp src/rx_afxdp.cpp src/rx_dpdk.cpp)
target_link_libraries(bl_rx PRIVATE bl_core Threads::Threads)
target_compile_definitions(bl_rx PRIVATE BL_DEFAULT_BPF_OBJ="${BL_BPF_OBJ}")
if(BL_HAVE_XDP)
  target_compile_definitions(bl_rx PRIVATE BL_HAVE_XDP=1)
  target_link_libraries(bl_rx PRIVATE PkgConfig::LIBXDP PkgConfig::LIBBPF)
  add_dependencies(bl_rx bl_bpf)
endif()
if(BL_HAVE_DPDK)
  target_compile_definitions(bl_rx PRIVATE BL_HAVE_DPDK=1)
  target_link_libraries(bl_rx PRIVATE PkgConfig::LIBDPDK)
  # DPDK headers are not -Wconversion/-Wpedantic clean.
  set_source_files_properties(src/rx_dpdk.cpp PROPERTIES COMPILE_OPTIONS
                              "-Wno-conversion;-Wno-pedantic;-Wno-shadow")
endif()

add_executable(bl_decoder_bench bench/decoder_bench.cpp)
target_link_libraries(bl_decoder_bench PRIVATE bl_core)

add_executable(bl_gen_sample tools/gen_sample.cpp)
target_compile_options(bl_gen_sample PRIVATE -O2)

# --- tests -----------------------------------------------------------------
enable_testing()
add_executable(bl_tests tests/test_main.cpp)
target_link_libraries(bl_tests PRIVATE bl_core)
add_test(NAME unit COMMAND bl_tests ${CMAKE_SOURCE_DIR}/data/sample.NASDAQ_ITCH50)

# Each CASE>0 must fail to compile *with its own static_assert message*;
# CASE 0 must compile. Matching the message stops a typo from passing.
set(BL_STATIC_EXPECT
    ""
    "field runs past the end"
    "two fields overlap"
    "share a type byte"
    "different struct"
    "wider than its destination"
    "must be 1, 2, 4, 6 or 8 bytes"
    "offset 0 is the type byte")
foreach(case RANGE 0 7)
  add_test(NAME schema_static_${case}
           COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only
                   -I${CMAKE_SOURCE_DIR}/include -DCASE=${case}
                   ${CMAKE_SOURCE_DIR}/tests/schema_static_checks.cpp)
  if(NOT case EQUAL 0)
    list(GET BL_STATIC_EXPECT ${case} expect)
    set_tests_properties(schema_static_${case} PROPERTIES PASS_REGULAR_EXPRESSION "${expect}")
  endif()
endforeach()
