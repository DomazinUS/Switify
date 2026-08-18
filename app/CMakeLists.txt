cmake_minimum_required(VERSION 3.13)

# Switify — standalone Borealis app (Phase 1).
# Build for Switch:  ..\build-app.ps1   (PLATFORM_SWITCH)
# Build for desktop: cmake -B build -G Ninja -DPLATFORM_DESKTOP=ON

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/borealis/library)

include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(Switify)

# The borealis toolchain path only applies __SWITCH__ and the devkitA64
# CPU/TLS flags to the borealis target itself (via BRLS_PLATFORM_OPTION).
# bell/cspot/app must compile with the same define and flags, or platform
# branches (WrappedSemaphore, mdnsd pipe, NXAudioSink selection) silently
# pick the wrong side and TLS models mismatch libnx.
if (PLATFORM_SWITCH)
    add_compile_definitions(__SWITCH__)
    add_compile_options(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec)
endif ()

set(VERSION_MAJOR "0")
set(VERSION_MINOR "6")
set(VERSION_ALTER "4")
set(PROJECT_AUTHOR "HiroshiYamauchi")
set(PROJECT_TITLEID "010ff000ffff0117")
set(PACKAGE_NAME "app.switify")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

# --- cspot (same configuration the POC validated on hardware)
set(CSPOT_USE_PREGEN_PROTO ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_SINKS     ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_WEBSERVER ON  CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT      ON  CACHE BOOL "" FORCE)
set(BELL_CODEC_VORBIS      ON  CACHE BOOL "" FORCE)
# Switify explicitly requests Spotify's OGG Vorbis streams. Keeping unrelated
# decoders resident only increases the NRO image and working set.
set(BELL_CODEC_AAC         OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_MP3         OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_ALAC        OFF CACHE BOOL "" FORCE)
set(BELL_CODEC_OPUS        OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../vendor/cspot ${CMAKE_BINARY_DIR}/cspot)

# --- borealis
add_subdirectory(${BOREALIS_LIBRARY} ${CMAKE_BINARY_DIR}/borealis)

# --- app sources (plus the engine pieces shared with the POC target)
file(GLOB MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(APPEND MAIN_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/NXPlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/NXConsoleLog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/ZeroconfHttpServer.cpp
)
if (PLATFORM_SWITCH)
    list(APPEND MAIN_SRC
        ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch/NXAudioSink.cpp
    )
endif ()

program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../targets/switch
)
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(${PROJECT_NAME} PRIVATE cspot borealis)
if (PLATFORM_SWITCH)
    find_package(CURL REQUIRED)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CURL_STATICLIB)
    target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CURL_LIBRARIES} z m)
endif ()

# --- NRO packaging (same recipe as the borealis demo / twiNX)
if (PLATFORM_SWITCH)
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        # copy_directory merges into an existing destination, so remove it
        # first to prevent deleted prototype resources surviving in the NRO.
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/resources/font
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif ()
