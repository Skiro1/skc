cmake_minimum_required(VERSION 3.20)
project(skc VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Public API (header-only) ───────────────────────────────────────────
add_library(libskc INTERFACE)
target_include_directories(libskc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(libskc INTERFACE cxx_std_17)

# Shared implementation (skc compression + dynamic zstd loader)
set(SKC_SOURCES
    src/codec.cpp
    src/zstd_wrapper.cpp
)

# ── CLI tool (skcconv) ─────────────────────────────────────────────────
add_executable(skcconv ${SKC_SOURCES} src/skcconv.cpp)
target_include_directories(skcconv PRIVATE include src)
target_link_libraries(skcconv PRIVATE libskc)

# ── Tests ──────────────────────────────────────────────────────────────
enable_testing()
add_executable(skc_test ${SKC_SOURCES} test/main.cpp)
target_include_directories(skc_test PRIVATE include src)
target_link_libraries(skc_test PRIVATE libskc)
add_test(NAME skc_test COMMAND skc_test)

# Copy libzstd.dll next to built binaries (runtime dynamic load)
find_file(ZSTD_DLL NAMES libzstd.dll
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/lib/zstd/zstd-v1.5.7-win64/dll"
    NO_DEFAULT_PATH)
if(ZSTD_DLL)
    add_custom_command(TARGET skcconv POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ZSTD_DLL}" "$<TARGET_FILE_DIR:skcconv>")
    add_custom_command(TARGET skc_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ZSTD_DLL}" "$<TARGET_FILE_DIR:skc_test>")
else()
    message(WARNING "libzstd.dll not found; runtime compression needs it on PATH")
endif()
