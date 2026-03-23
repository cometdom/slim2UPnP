cmake_minimum_required(VERSION 3.10)
project(slim2upnp VERSION 0.0.2)

set(CMAKE_CXX_STANDARD 17)

# ============================================
# Architecture Detection
# ============================================

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  slim2UPnP - Build Configuration")
message(STATUS "═══════════════════════════════════════════════════════")

execute_process(
    COMMAND uname -m
    OUTPUT_VARIABLE UNAME_M
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(UNAME_M STREQUAL "x86_64")
    set(BASE_ARCH "x64")
    set(ARCH_DESC_BASE "x86_64 (Intel/AMD 64-bit)")
elseif(UNAME_M STREQUAL "aarch64" OR UNAME_M STREQUAL "arm64")
    set(BASE_ARCH "aarch64")
    set(ARCH_DESC_BASE "ARM64 (aarch64)")
elseif(UNAME_M STREQUAL "riscv64")
    set(BASE_ARCH "riscv64")
    set(ARCH_DESC_BASE "RISC-V 64-bit")
else()
    set(BASE_ARCH "unknown")
    set(ARCH_DESC_BASE "Unknown: ${UNAME_M}")
endif()

message(STATUS "Architecture: ${ARCH_DESC_BASE}")

# ============================================
# Architecture-Specific Variant Detection
# ============================================

if(BASE_ARCH STREQUAL "x64")
    execute_process(
        COMMAND grep -q avx2 /proc/cpuinfo
        RESULT_VARIABLE HAS_AVX2_RESULT
    )
    execute_process(
        COMMAND grep -q avx512 /proc/cpuinfo
        RESULT_VARIABLE HAS_AVX512_RESULT
    )
    execute_process(
        COMMAND sh -c "lscpu 2>/dev/null | grep -qi 'AMD.*Zen 4'"
        RESULT_VARIABLE IS_ZEN4_RESULT
    )

    if(IS_ZEN4_RESULT EQUAL 0)
        set(CPU_DESC "AMD Zen 4 detected")
    elseif(HAS_AVX512_RESULT EQUAL 0)
        set(CPU_DESC "AVX512 detected (x86-64-v4)")
    elseif(HAS_AVX2_RESULT EQUAL 0)
        set(CPU_DESC "AVX2 detected (x86-64-v3)")
    else()
        set(CPU_DESC "Basic x64 (x86-64-v2)")
    endif()

elseif(BASE_ARCH STREQUAL "aarch64")
    set(CPU_DESC "ARM64")
endif()

message(STATUS "CPU: ${CPU_DESC}")

# ============================================
# Find Required Packages
# ============================================

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# --- libFLAC (required) ---
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FLAC flac)
endif()
if(NOT FLAC_FOUND)
    find_library(FLAC_LIB NAMES FLAC)
    find_path(FLAC_INC NAMES FLAC/stream_decoder.h)
    if(FLAC_LIB AND FLAC_INC)
        set(FLAC_LIBRARIES ${FLAC_LIB})
        set(FLAC_INCLUDE_DIRS ${FLAC_INC})
        set(FLAC_FOUND TRUE)
        message(STATUS "libFLAC found via find_library: ${FLAC_LIB}")
    else()
        message(FATAL_ERROR
            "libFLAC not found! Install with:\n"
            "  Ubuntu/Debian: sudo apt-get install libflac-dev\n"
            "  Fedora: sudo dnf install flac-devel\n"
            "  Arch: sudo pacman -S flac\n"
        )
    endif()
endif()

# --- libupnp (required) ---
if(PKG_CONFIG_FOUND)
    pkg_check_modules(UPNP libupnp)
endif()
if(NOT UPNP_FOUND)
    find_library(UPNP_LIB NAMES upnp)
    find_library(IXML_LIB NAMES ixml)
    find_path(UPNP_INC NAMES upnp/upnp.h)
    if(UPNP_LIB AND IXML_LIB AND UPNP_INC)
        set(UPNP_LIBRARIES ${UPNP_LIB} ${IXML_LIB})
        set(UPNP_INCLUDE_DIRS ${UPNP_INC})
        set(UPNP_FOUND TRUE)
        message(STATUS "libupnp found via find_library: ${UPNP_LIB}")
    else()
        message(FATAL_ERROR
            "libupnp not found! Install with:\n"
            "  Ubuntu/Debian: sudo apt-get install libupnp-dev\n"
            "  Fedora: sudo dnf install libupnp-devel\n"
            "  Arch: sudo pacman -S libupnp\n"
        )
    endif()
endif()

# ============================================
# Optional Codec: MP3 (libmpg123)
# ============================================
option(ENABLE_MP3 "Enable MP3 decoding via libmpg123" ON)
if(ENABLE_MP3)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(MPG123 libmpg123)
    endif()
    if(NOT MPG123_FOUND)
        find_library(MPG123_LIB NAMES mpg123)
        find_path(MPG123_INC NAMES mpg123.h)
        if(MPG123_LIB AND MPG123_INC)
            set(MPG123_LIBRARIES ${MPG123_LIB})
            set(MPG123_INCLUDE_DIRS ${MPG123_INC})
            set(MPG123_FOUND TRUE)
        endif()
    endif()
    if(MPG123_FOUND)
        message(STATUS "MP3 support: ENABLED (libmpg123)")
        add_definitions(-DENABLE_MP3)
    else()
        message(STATUS "MP3 support: DISABLED (libmpg123-dev not found)")
        set(ENABLE_MP3 OFF)
    endif()
endif()

# ============================================
# Optional Codec: Ogg Vorbis (libvorbisfile)
# ============================================
option(ENABLE_OGG "Enable Ogg Vorbis decoding via libvorbisfile" ON)
if(ENABLE_OGG)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(VORBISFILE vorbisfile)
    endif()
    if(NOT VORBISFILE_FOUND)
        find_library(VORBISFILE_LIB NAMES vorbisfile)
        find_library(VORBIS_LIB NAMES vorbis)
        find_library(OGG_LIB NAMES ogg)
        find_path(VORBIS_INC NAMES vorbis/vorbisfile.h)
        if(VORBISFILE_LIB AND VORBIS_LIB AND OGG_LIB AND VORBIS_INC)
            set(VORBISFILE_LIBRARIES ${VORBISFILE_LIB} ${VORBIS_LIB} ${OGG_LIB})
            set(VORBISFILE_INCLUDE_DIRS ${VORBIS_INC})
            set(VORBISFILE_FOUND TRUE)
        endif()
    endif()
    if(VORBISFILE_FOUND)
        message(STATUS "Ogg Vorbis support: ENABLED (libvorbisfile)")
        add_definitions(-DENABLE_OGG)
    else()
        message(STATUS "Ogg Vorbis support: DISABLED (libvorbis-dev not found)")
        set(ENABLE_OGG OFF)
    endif()
endif()

# ============================================
# Optional Codec: AAC (fdk-aac)
# ============================================
option(ENABLE_AAC "Enable AAC decoding via fdk-aac" ON)
if(ENABLE_AAC)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FDKAAC fdk-aac)
    endif()
    if(NOT FDKAAC_FOUND)
        find_library(FDKAAC_LIB NAMES fdk-aac)
        find_path(FDKAAC_INC NAMES fdk-aac/aacdecoder_lib.h)
        if(FDKAAC_LIB AND FDKAAC_INC)
            set(FDKAAC_LIBRARIES ${FDKAAC_LIB})
            set(FDKAAC_INCLUDE_DIRS ${FDKAAC_INC})
            set(FDKAAC_FOUND TRUE)
        endif()
    endif()
    if(FDKAAC_FOUND)
        message(STATUS "AAC support: ENABLED (fdk-aac)")
        add_definitions(-DENABLE_AAC)
    else()
        message(STATUS "AAC support: DISABLED (libfdk-aac-dev not found)")
        set(ENABLE_AAC OFF)
    endif()
endif()

# ============================================
# Optional: FFmpeg backend (libavcodec/libavutil)
# ============================================
option(ENABLE_FFMPEG "Enable FFmpeg decoder backend" ON)
if(ENABLE_FFMPEG)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(AVCODEC libavcodec)
        pkg_check_modules(AVUTIL libavutil)
    endif()
    if(NOT AVCODEC_FOUND)
        find_library(AVCODEC_LIB NAMES avcodec)
        find_path(AVCODEC_INC NAMES libavcodec/avcodec.h
                  PATHS /usr/include /usr/include/x86_64-linux-gnu /usr/include/aarch64-linux-gnu)
        if(AVCODEC_LIB AND AVCODEC_INC)
            set(AVCODEC_LIBRARIES ${AVCODEC_LIB})
            set(AVCODEC_INCLUDE_DIRS ${AVCODEC_INC})
            set(AVCODEC_FOUND TRUE)
        endif()
    endif()
    if(NOT AVUTIL_FOUND)
        find_library(AVUTIL_LIB NAMES avutil)
        if(AVUTIL_LIB)
            set(AVUTIL_LIBRARIES ${AVUTIL_LIB})
            set(AVUTIL_FOUND TRUE)
        endif()
    endif()
    if(AVCODEC_FOUND AND AVUTIL_FOUND)
        message(STATUS "FFmpeg backend: ENABLED (--decoder ffmpeg)")
        add_definitions(-DENABLE_FFMPEG)
    else()
        message(STATUS "FFmpeg backend: DISABLED (libavcodec-dev/libavutil-dev not found)")
        set(ENABLE_FFMPEG OFF)
    endif()
endif()

# ============================================
# Include Directories
# ============================================

set(EXTRA_INCLUDE_DIRS ${FLAC_INCLUDE_DIRS} ${UPNP_INCLUDE_DIRS})
if(ENABLE_MP3)
    list(APPEND EXTRA_INCLUDE_DIRS ${MPG123_INCLUDE_DIRS})
endif()
if(ENABLE_OGG)
    list(APPEND EXTRA_INCLUDE_DIRS ${VORBISFILE_INCLUDE_DIRS})
endif()
if(ENABLE_AAC)
    list(APPEND EXTRA_INCLUDE_DIRS ${FDKAAC_INCLUDE_DIRS})
endif()
if(ENABLE_FFMPEG)
    list(APPEND EXTRA_INCLUDE_DIRS ${AVCODEC_INCLUDE_DIRS})
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${EXTRA_INCLUDE_DIRS}
)

# ============================================
# Link Directories
# ============================================

set(EXTRA_LINK_DIRS ${FLAC_LIBRARY_DIRS} ${UPNP_LIBRARY_DIRS})
if(ENABLE_MP3)
    list(APPEND EXTRA_LINK_DIRS ${MPG123_LIBRARY_DIRS})
endif()
if(ENABLE_OGG)
    list(APPEND EXTRA_LINK_DIRS ${VORBISFILE_LIBRARY_DIRS})
endif()
if(ENABLE_AAC)
    list(APPEND EXTRA_LINK_DIRS ${FDKAAC_LIBRARY_DIRS})
endif()
if(ENABLE_FFMPEG)
    list(APPEND EXTRA_LINK_DIRS ${AVCODEC_LIBRARY_DIRS} ${AVUTIL_LIBRARY_DIRS})
endif()

link_directories(${EXTRA_LINK_DIRS})

# ============================================
# Sources
# ============================================

set(SLIM2UPNP_SOURCES
    src/main.cpp
    src/SlimprotoClient.cpp
    src/HttpStreamClient.cpp
    src/Decoder.cpp
    src/FlacDecoder.cpp
    src/PcmDecoder.cpp
    src/DsdProcessor.cpp
    src/DsdStreamReader.cpp
    src/AudioHttpServer.cpp
    src/UPnPController.cpp
)

if(ENABLE_MP3)
    list(APPEND SLIM2UPNP_SOURCES src/Mp3Decoder.cpp)
endif()
if(ENABLE_OGG)
    list(APPEND SLIM2UPNP_SOURCES src/OggDecoder.cpp)
endif()
if(ENABLE_AAC)
    list(APPEND SLIM2UPNP_SOURCES src/AacDecoder.cpp)
endif()
if(ENABLE_FFMPEG)
    list(APPEND SLIM2UPNP_SOURCES src/FfmpegDecoder.cpp)
endif()

# ============================================
# Create Executable
# ============================================

add_executable(slim2upnp ${SLIM2UPNP_SOURCES})

# ============================================
# Static Build Option
# ============================================

option(STATIC_BUILD "Build a statically linked binary" OFF)
if(STATIC_BUILD)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
    target_link_options(slim2upnp PRIVATE -static)
    message(STATUS "Static build: ENABLED")
endif()

# ============================================
# Link Libraries
# ============================================

target_link_libraries(slim2upnp
    ${CMAKE_THREAD_LIBS_INIT}
    ${FLAC_LIBRARIES}
    ${UPNP_LIBRARIES}
)

if(ENABLE_MP3)
    target_link_libraries(slim2upnp ${MPG123_LIBRARIES})
endif()
if(ENABLE_OGG)
    target_link_libraries(slim2upnp ${VORBISFILE_LIBRARIES})
endif()
if(ENABLE_AAC)
    target_link_libraries(slim2upnp ${FDKAAC_LIBRARIES})
endif()
if(ENABLE_FFMPEG)
    target_link_libraries(slim2upnp ${AVCODEC_LIBRARIES} ${AVUTIL_LIBRARIES})
endif()

# ============================================
# Compiler Flags
# ============================================

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Architecture-specific SIMD flags
if(BASE_ARCH STREQUAL "x64")
    if(DEFINED TARGET_MARCH)
        set(MARCH_LEVEL ${TARGET_MARCH})
    elseif(IS_ZEN4_RESULT EQUAL 0)
        set(MARCH_LEVEL "zen4")
    elseif(HAS_AVX512_RESULT EQUAL 0)
        set(MARCH_LEVEL "v4")
    elseif(HAS_AVX2_RESULT EQUAL 0)
        set(MARCH_LEVEL "v3")
    else()
        set(MARCH_LEVEL "v2")
    endif()

    if(MARCH_LEVEL STREQUAL "zen4")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=znver4 -mtune=znver4")
    elseif(MARCH_LEVEL STREQUAL "v4")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v4 -mavx512f -mavx512bw -mavx512vl -mavx512dq")
    elseif(MARCH_LEVEL STREQUAL "v3")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v3 -mavx2 -mfma")
    elseif(MARCH_LEVEL STREQUAL "v2")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v2")
    elseif(MARCH_LEVEL STREQUAL "native")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
    endif()
    message(STATUS "SIMD: ${MARCH_LEVEL}")

elseif(BASE_ARCH STREQUAL "aarch64")
    if(DEFINED TARGET_MARCH AND NOT TARGET_MARCH STREQUAL "native")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=${TARGET_MARCH}")
    else()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=native")
    endif()
    message(STATUS "SIMD: ARM64 native")
endif()

# ============================================
# Production Build (NOLOG)
# ============================================

option(NOLOG "Disable verbose logging for production builds" OFF)
if(NOLOG)
    target_compile_definitions(slim2upnp PRIVATE NOLOG)
    message(STATUS "NOLOG: Verbose logging disabled (production build)")
endif()

# ============================================
# Install
# ============================================

install(TARGETS slim2upnp DESTINATION bin)

# ============================================
# Print Summary
# ============================================

message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "  Build Configuration Summary")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
message(STATUS "System:")
message(STATUS "  uname -m:       ${UNAME_M}")
message(STATUS "  Base Arch:      ${BASE_ARCH}")
message(STATUS "")
message(STATUS "Codecs:")
message(STATUS "  FLAC:           ENABLED (always)")
message(STATUS "  PCM:            ENABLED (always)")
if(ENABLE_MP3)
    message(STATUS "  MP3:            ENABLED (libmpg123)")
else()
    message(STATUS "  MP3:            DISABLED")
endif()
if(ENABLE_OGG)
    message(STATUS "  Ogg Vorbis:     ENABLED (libvorbisfile)")
else()
    message(STATUS "  Ogg Vorbis:     DISABLED")
endif()
if(ENABLE_AAC)
    message(STATUS "  AAC:            ENABLED (fdk-aac)")
else()
    message(STATUS "  AAC:            DISABLED")
endif()
message(STATUS "")
message(STATUS "Backends:")
if(ENABLE_FFMPEG)
    message(STATUS "  FFmpeg:         ENABLED (--decoder ffmpeg)")
else()
    message(STATUS "  FFmpeg:         DISABLED")
endif()
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  libFLAC:        ${FLAC_LIBRARIES}")
message(STATUS "  libupnp:        ${UPNP_LIBRARIES}")
message(STATUS "")
message(STATUS "Build:")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER}")
message(STATUS "  Target:         slim2upnp")
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════")
message(STATUS "")
