cmake_minimum_required(VERSION 3.13)

project(switify-background C CXX)

option(SWITIFY_LIGHTWEIGHT_OVERLAY
       "Build the reduced-resource overlay background service" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "Use the devkitPro Nintendo Switch toolchain")
endif()

add_compile_definitions(__SWITCH__ BELL_SWITCH_BOUNDED_STACKS)
add_compile_options(
  -march=armv8-a+crc+crypto
  -mtune=cortex-a57
  -mtp=soft
  -ftls-model=local-exec
  -ffunction-sections
  -fdata-sections
)
add_link_options(-Wl,--gc-sections)

set(CSPOT_USE_PREGEN_PROTO ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_WEBSERVER ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_CODEC_VORBIS ON CACHE BOOL "" FORCE)
# SpotifyEngine explicitly requests OGG Vorbis 160/320 and cspot falls back
# to OGG Vorbis 96. AAC, MP3, ALAC, and Opus cannot be selected by either the
# artwork-enabled or artwork-free service, so carrying those decoders only
# increases the resident image.
set(BELL_CODEC_AAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3 OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../vendor/cspot ${CMAKE_CURRENT_BINARY_DIR}/cspot)

add_executable(switify-background
  engine/main.cpp
  engine/background_backend.cpp
  source/ipc_server.c
  source/ipc_self_test.cpp
  source/switify_service.cpp
  ../common/source/service_core.cpp
  ../../app/src/spotify_engine.cpp
  ../../app/src/app_settings.cpp
  ../../targets/switch/NXAudioSink.cpp
  ../../targets/switch/NXConsoleLog.cpp
  ../../targets/switch/NXPlayer.cpp
  ../../targets/switch/ZeroconfHttpServer.cpp
)

target_include_directories(switify-background PRIVATE
  engine
  source
  ../common/include
  ../../app/src
  ../../targets/switch
)

find_package(CURL REQUIRED)
target_compile_definitions(switify-background PRIVATE
  CURL_STATICLIB
  SWITIFY_BACKGROUND_SERVICE
)
if(SWITIFY_LIGHTWEIGHT_OVERLAY)
  target_compile_definitions(switify-background PRIVATE
    SWITIFY_LIGHTWEIGHT_OVERLAY=1)
endif()
target_include_directories(switify-background PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(switify-background PRIVATE
  cspot
  ${CURL_LIBRARIES}
  z
  m
  nx
)

find_program(NPDMTOOL npdmtool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(ELF2NSO elf2nso HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
find_program(BUILD_PFS0 build_pfs0 HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)

set(PACKAGE_DIR ${CMAKE_CURRENT_BINARY_DIR}/package)
set(PACKAGE_NPDM ${CMAKE_CURRENT_BINARY_DIR}/switify-background.npdm)
set(PACKAGE_NSO ${CMAKE_CURRENT_BINARY_DIR}/switify-background.nso)
set(PACKAGE_NSP ${CMAKE_CURRENT_BINARY_DIR}/switify-background.nsp)

add_custom_command(
  OUTPUT ${PACKAGE_NPDM}
  COMMAND ${NPDMTOOL} ${CMAKE_CURRENT_SOURCE_DIR}/switify-sysmodule.json
          ${PACKAGE_NPDM}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/switify-sysmodule.json
  VERBATIM
)

add_custom_command(
  OUTPUT ${PACKAGE_NSO}
  COMMAND ${ELF2NSO} $<TARGET_FILE:switify-background> ${PACKAGE_NSO}
  DEPENDS switify-background
  VERBATIM
)

add_custom_command(
  OUTPUT ${PACKAGE_NSP}
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${PACKAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PACKAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${PACKAGE_NSO} ${PACKAGE_DIR}/main
  COMMAND ${CMAKE_COMMAND} -E copy ${PACKAGE_NPDM} ${PACKAGE_DIR}/main.npdm
  COMMAND ${BUILD_PFS0} ${PACKAGE_DIR} ${PACKAGE_NSP}
  DEPENDS ${PACKAGE_NSO} ${PACKAGE_NPDM}
  VERBATIM
)

add_custom_target(switify-background-package ALL DEPENDS ${PACKAGE_NSP})
