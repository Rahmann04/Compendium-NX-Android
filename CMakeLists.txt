# Compendium: install and manage Persona mods on a modded Switch.
#
# Three ways to build:
#
#   Core + unit tests + compcli dev tool (host):
#     cmake -S . -B build && cmake --build build && ./build/comptests
#
#   The app on Linux/macOS (for UI development, with a fake SD card):
#     cmake -S . -B build-desktop -DPLATFORM_DESKTOP=ON && cmake --build build-desktop
#     MM_SD_ROOT=/tmp/fake-sd ./build-desktop/Compendium
#
#   The app for Switch (.nro), with devkitPro:
#     DEVKITPRO=/opt/devkitpro cmake -S . -B build-switch -DPLATFORM_SWITCH=ON
#     cmake --build build-switch --target Compendium.nro
cmake_minimum_required(VERSION 3.15)

set(APP_BUILD OFF)
if(PLATFORM_DESKTOP OR PLATFORM_SWITCH OR PLATFORM_ANDROID)
    set(APP_BUILD ON)
    set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library)
    include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
    option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
    cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
    cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
    cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)
    include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)
endif()

file(STRINGS VERSION PMM_VERSION LIMIT_COUNT 1)
project(Compendium VERSION ${PMM_VERSION} LANGUAGES C CXX)
set(APP_TITLE "Compendium")
set(APP_AUTHOR "SantiBros")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- core library
file(GLOB CORE_SOURCES source/core/*.cpp)
add_library(mmcore STATIC ${CORE_SOURCES})
target_sources(mmcore PRIVATE source/core/cpktoolkit/cpk.cpp source/core/cpktoolkit/utf.cpp source/core/cpktoolkit/crilayla.cpp)
target_include_directories(mmcore PUBLIC source/core source/third_party)
target_compile_options(mmcore PRIVATE -Wall -Wextra)

if(NINTENDO_SWITCH OR PLATFORM_SWITCH)
    # Static portlibs; private deps listed explicitly (pkg-config may see host .pc files).
    target_include_directories(mmcore PUBLIC $ENV{DEVKITPRO}/portlibs/switch/include)
    target_link_directories(mmcore PUBLIC $ENV{DEVKITPRO}/portlibs/switch/lib)
    target_link_libraries(mmcore PUBLIC curl mbedtls mbedx509 mbedcrypto archive zstd lz4 lzma bz2 expat z)
elseif(PLATFORM_ANDROID)
    # Native Android ARM64 static dependencies
    set(ANDROID_DEPS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/android-deps/install"
        CACHE PATH "Android ARM64 dependency install prefix"
    )

    add_library(android_zlib STATIC IMPORTED GLOBAL)
    set_target_properties(android_zlib PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libz.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_lzma STATIC IMPORTED GLOBAL)
    set_target_properties(android_lzma PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/liblzma.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_zstd STATIC IMPORTED GLOBAL)
    set_target_properties(android_zstd PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libzstd.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_lz4 STATIC IMPORTED GLOBAL)
    set_target_properties(android_lz4 PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/liblz4.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_bz2 STATIC IMPORTED GLOBAL)
    set_target_properties(android_bz2 PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libbz2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_archive STATIC IMPORTED GLOBAL)
    set_target_properties(android_archive PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libarchive.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_crypto STATIC IMPORTED GLOBAL)
    set_target_properties(android_crypto PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_ssl STATIC IMPORTED GLOBAL)
    set_target_properties(android_ssl PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_curl STATIC IMPORTED GLOBAL)
    set_target_properties(android_curl PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libcurl.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    add_library(android_ssh2 STATIC IMPORTED GLOBAL)
    set_target_properties(android_ssh2 PROPERTIES
        IMPORTED_LOCATION "${ANDROID_DEPS}/lib/libssh2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ANDROID_DEPS}/include")

    target_link_libraries(mmcore PUBLIC
        android_archive
        android_bz2
        android_lz4
        android_lzma
        android_zstd
        android_curl
        android_ssh2
        android_ssl
        android_crypto
        android_zlib
        m
        dl
    )
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
    find_package(CURL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(mmcore PUBLIC PkgConfig::LIBARCHIVE CURL::libcurl ZLIB::ZLIB)
endif()

# ---------------------------------------------------------------- host tools
if(NOT APP_BUILD)
    file(GLOB TEST_SOURCES tests/*.cpp)
    add_executable(comptests ${TEST_SOURCES})
    target_link_libraries(comptests PRIVATE mmcore)
    target_compile_definitions(comptests PRIVATE MM_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
    enable_testing()
    add_test(NAME comptests COMMAND comptests)

    add_executable(compcli tools/compcli.cpp)
    target_link_libraries(compcli PRIVATE mmcore)
    return()
endif()

# ---------------------------------------------------------------- the app
add_subdirectory(library/borealis/library)
find_package(Threads REQUIRED)
if(TARGET yogacore)
    target_compile_options(yogacore PRIVATE -frtti)
endif()

set(APP_RESOURCES ${CMAKE_BINARY_DIR}/resources)
set(BRLS_RES ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/resources)
add_custom_target(app_resources
    COMMAND ${CMAKE_COMMAND} -E make_directory ${APP_RESOURCES}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources ${APP_RESOURCES}
)
if(PLATFORM_ANDROID)
    add_custom_command(TARGET app_resources POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES} ${APP_RESOURCES})
else()
    add_custom_command(TARGET app_resources POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/material ${APP_RESOURCES}/material
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/i18n/en-US ${APP_RESOURCES}/i18n/en-US
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/img/sys ${APP_RESOURCES}/img/sys)
endif()
if(PLATFORM_DESKTOP)
    add_custom_command(TARGET app_resources POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${BRLS_RES}/font ${APP_RESOURCES}/font)
endif()

file(GLOB_RECURSE APP_SOURCES source/app/*.cpp)
list(APPEND APP_SOURCES source/third_party/qrcodegen/qrcodegen.cpp)
set(APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})
if(PLATFORM_ANDROID)
    set(BOREALIS_WEBP_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/android-project/app/src/main/jniLibs/arm64-v8a")
    add_library(android_webp SHARED IMPORTED GLOBAL)
    set_target_properties(android_webp PROPERTIES
        IMPORTED_LOCATION "${BOREALIS_WEBP_DIR}/libwebp.so"
        INTERFACE_INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webp/include"
    )
    list(APPEND APP_PLATFORM_LIB android_webp)
endif()

if(PLATFORM_DESKTOP)
    pkg_check_modules(LIBWEBP REQUIRED IMPORTED_TARGET libwebp)
    list(APPEND APP_PLATFORM_LIB PkgConfig::LIBWEBP)
endif()
if(PLATFORM_SWITCH)
    list(APPEND APP_SOURCES ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
    list(APPEND APP_PLATFORM_LIB webp glfw3 EGL glapi drm_nouveau nx m)
endif()

program_target(Compendium "${APP_SOURCES}")
add_dependencies(Compendium app_resources)
target_include_directories(Compendium PRIVATE source/app)
target_compile_options(Compendium PRIVATE -ffunction-sections -fdata-sections -Wall)
target_compile_definitions(mmcore PRIVATE $<$<BOOL:${APP_BUILD}>:MM_ANDROID_APP>)

target_compile_definitions(Compendium PRIVATE APP_VERSION="${PROJECT_VERSION}" APP_TITLE="${APP_TITLE}")

target_link_libraries(Compendium PRIVATE mmcore borealis ${APP_PLATFORM_LIB})

if(PLATFORM_ANDROID)
    find_program(ANDROID_PATCHELF patchelf)
    find_program(ANDROID_READELF readelf)

    if(NOT ANDROID_PATCHELF OR NOT ANDROID_READELF)
        message(FATAL_ERROR "patchelf dan readelf diperlukan untuk pembersihan ELF Android.")
    endif()

    add_custom_command(TARGET Compendium POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DTARGET_FILE=$<TARGET_FILE:Compendium>"
            "-DPATCHELF=${ANDROID_PATCHELF}"
            "-DREADELF=${ANDROID_READELF}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/android_fix_elf.cmake"
        VERBATIM
        COMMENT "Memperbaiki dependensi dan membersihkan RUNPATH Android"
    )
endif()


if(PLATFORM_SWITCH)
    set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/img/icon.jpg)
    add_custom_target(Compendium.nro DEPENDS Compendium app_resources
        COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${APP_AUTHOR}" "${PROJECT_VERSION}" Compendium.nacp
        COMMAND ${NX_ELF2NRO_EXE} Compendium.elf Compendium.nro --icon=${APP_ICON}
                --nacp=Compendium.nacp --romfsdir=${APP_RESOURCES}
    )
endif()
