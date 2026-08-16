cmake_minimum_required(VERSION 3.13)

# Switify POC — Nintendo Switch target.
#
# Build with the devkitPro toolchain file:
#   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
#   cmake --build build
#
# Requires devkitA64, libnx, switch-mbedtls (pacman -S switch-mbedtls) and
# the devkitPro cmake support (pacman -S switch-cmake or dkp-cmake).

project(switify-poc CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "This target must be built with the devkitPro Switch toolchain file: -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

# --- cspot/bell configuration -------------------------------------------------
# Keep the footprint minimal: Vorbis only, no built-in sinks (we provide our
# own audout sink), no civetweb webserver (we provide a minimal zeroconf HTTP
# server), no MQTT, and pregenerated protobuf sources so the build does not
# depend on python/protoc.
set(CSPOT_USE_PREGEN_PROTO ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_SINKS     ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_WEBSERVER ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT      ON  CACHE BOOL "" FORCE)
# All codecs stay ON: bell's codec wrapper headers include every decoder
# unconditionally (mp3dec.h, pvmp4audiodecoder_api.h, ...), so disabling
# individual codecs breaks the build. They are all vendored plain C/C++.
set(BELL_CODEC_VORBIS      ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_AAC         ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3         ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC        ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS        ON  CACHE BOOL "" FORCE)

add_subdirectory(../../vendor/cspot ${CMAKE_CURRENT_BINARY_DIR}/cspot)

# --- POC executable -----------------------------------------------------------
add_executable(switify-poc
    main.cpp
    NXAudioSink.cpp
    NXConsoleLog.cpp
    NXPlayer.cpp
    ZeroconfHttpServer.cpp
)

target_link_libraries(switify-poc PRIVATE cspot nx m)
target_include_directories(switify-poc PRIVATE ".")

# --- NRO packaging ------------------------------------------------------------
if(COMMAND nx_generate_nro)
    nx_create_nacp(switify-poc.nacp
        NAME    "Switify POC"
        AUTHOR  "HiroshiYamauchi"
        VERSION "0.1.0"
    )
    nx_generate_nro(switify-poc
        NACP switify-poc.nacp
    )
else()
    # Fallback for environments without dkp-cmake's NRO helpers
    find_program(ELF2NRO elf2nro HINTS "$ENV{DEVKITPRO}/tools/bin")
    find_program(NACPTOOL nacptool HINTS "$ENV{DEVKITPRO}/tools/bin")
    add_custom_command(TARGET switify-poc POST_BUILD
        COMMAND ${NACPTOOL} --create "Switify POC" "HiroshiYamauchi" "0.1.0" switify-poc.nacp
        COMMAND ${ELF2NRO} $<TARGET_FILE:switify-poc> switify-poc.nro --nacp=switify-poc.nacp
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Building switify-poc.nro"
    )
endif()
