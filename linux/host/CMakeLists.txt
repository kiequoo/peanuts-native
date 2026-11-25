# dmhost-linux: the Horizon host (dm/host) and the NVN → Vulkan replay (dm/gfx/vk_android) for arm64 Linux (glibc, Wayland).
# This is the Stage 1 selftest build: it produces a small launcher that opens a Wayland window and runs nxhost_vk_selftest.
# Full game builds add the patcher target (--target linux) in Stage 2 and game-specific profiles. See linux/PORTING.md.
#
# Configure and build (inside a distrobox or on the Odin 3 itself):
#   cmake -S linux/host -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build/linux --target selftest_launcher
# Run:
#   NXHOST_VK_SELFTEST=600 ./build/linux/selftest_launcher
cmake_minimum_required(VERSION 3.22)
project(dmhost_linux C ASM)

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64"))
  message(WARNING "This build is for arm64 Linux; targeting ${CMAKE_SYSTEM_PROCESSOR}/${CMAKE_SYSTEM_NAME}")
endif()

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(HOST ${ROOT}/dm/host)
set(GFX ${ROOT}/dm/gfx)
set(TP ${ROOT}/dm/third_party)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Mach-O assembly → ELF assembly (same script as the Android build)
set(ASM_CONV ${ROOT}/android/host/macho_asm_to_elf.py)
set(ASM_IN ${HOST}/entry.S ${GFX}/nvn/nvn_entry.S ${GFX}/nvn/nvn_thunks.S)
set(ASM_OUT)
foreach(s ${ASM_IN})
  get_filename_component(n ${s} NAME_WE)
  set(o ${GEN}/asm/${n}.S)
  add_custom_command(OUTPUT ${o}
    COMMAND ${Python3_EXECUTABLE} ${ASM_CONV} ${s} ${o}
    DEPENDS ${s} ${ASM_CONV})
  list(APPEND ASM_OUT ${o})
endforeach()

# --- Host C sources ---
# Every dm/host/*.c except the VideoToolbox movie decoder (no AVC on Linux yet; movie_avc_linux.c will stub it)
file(GLOB HOST_C CONFIGURE_DEPENDS ${HOST}/*.c)
list(REMOVE_ITEM HOST_C ${HOST}/hle_movie_avc.c ${HOST}/main.c)
# Linux platform, front end, input, and movie stubs (AVC/AAC → black; AV1 goes through dav1d below)
list(APPEND HOST_C
  ${HOST}/plat/plat_linux.c
  ${HOST}/plat/front_linux.c
  ${HOST}/android/input_android.c   # nxhost_input_* API; Android sensor/codec sections guarded by PLAT_ANDROID
  ${HOST}/android/input_map.c       # pure-C nxin_pad_* / nxin_touch_* helpers used by input_android.c
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs/movie_avc_stub.c   # Stage 2: replace with movie_avc_linux.c (FFmpeg H.264)
  ${ROOT}/dm/peanuts/profile/profile_rt.c
)

# --- GFX C sources ---
# NVN front end, vk_android replay, and the android_stub for Metal/GL entry points
file(GLOB GFX_C CONFIGURE_DEPENDS ${GFX}/*.c ${GFX}/nvn/*.c ${GFX}/tex/*.c ${GFX}/vk_android/*.c)

# --- Third-party libraries ---

# libvpx (VP8/VP9)
file(GLOB_RECURSE VPX_C CONFIGURE_DEPENDS ${TP}/libvpx/*.c)
add_library(vpx STATIC ${VPX_C})
target_include_directories(vpx PRIVATE ${TP}/libvpx ${TP}/libvpx/config)
target_compile_options(vpx PRIVATE -O2 -w)

# zstd decompressor
add_library(zstddec STATIC ${TP}/zstd/zstddeclib.c)
target_compile_options(zstddec PRIVATE -O2 -w -fvisibility=hidden)

# dav1d (AV1)
file(GLOB DAV1D_C CONFIGURE_DEPENDS ${TP}/dav1d/src/*.c ${TP}/dav1d/config/tmpl/*.c)
list(FILTER DAV1D_C EXCLUDE REGEX "_tmpl\\.c$")
list(APPEND DAV1D_C ${TP}/dav1d/src/arm/cpu.c)
file(GLOB DAV1D_S CONFIGURE_DEPENDS ${TP}/dav1d/src/arm/64/*.S)
list(FILTER DAV1D_S EXCLUDE REGEX "(_tmpl|/util)\\.S$")
add_library(dav1d STATIC ${DAV1D_C} ${DAV1D_S})
target_include_directories(dav1d PRIVATE ${ROOT}/android/host/dav1d_config ${TP}/dav1d ${TP}/dav1d/include ${TP}/dav1d/config)
target_compile_options(dav1d PRIVATE -O2 -w -fvisibility=hidden)
target_compile_definitions(dav1d PRIVATE _GNU_SOURCE)

# --- Game profile (generic stand-in for the selftest; Stage 2 replaces with a per-title profile) ---
# Use the PEANUTS generic profile header if available; otherwise generate an empty stand-in
set(PROFILE_DIR ${ROOT}/dm/peanuts/profile)
set(GENERIC_PROFILE ${PROFILE_DIR}/game_profile_generic.h)
set(PROFILE_INC ${GEN}/profile)
file(MAKE_DIRECTORY ${PROFILE_INC})
set(PROFILE_RT ${PROFILE_DIR}/profile_rt.h)
if(EXISTS ${GENERIC_PROFILE} AND EXISTS ${PROFILE_RT})
  configure_file(${GENERIC_PROFILE} ${PROFILE_INC}/game_profile.h COPYONLY)
  configure_file(${PROFILE_RT} ${PROFILE_INC}/profile_rt.h COPYONLY)
else()
  # Minimal stand-in: empty profile that leaves every game-specific macro undefined (the host's defaults apply)
  file(WRITE ${PROFILE_INC}/game_profile.h "/* generated: empty game profile for the Linux selftest build */\n")
endif()
# prewarm seed stand-in (no pipeline cache seeds for the selftest build)
if(EXISTS ${PROFILE_DIR}/vk_prewarm_seed_generic.h)
  configure_file(${PROFILE_DIR}/vk_prewarm_seed_generic.h ${PROFILE_INC}/vk_prewarm_seed.h COPYONLY)
else()
  file(WRITE ${PROFILE_INC}/vk_prewarm_seed.h "/* generated: no prewarm seeds for the Linux selftest build */\n")
endif()
# schema_stamp.h: DM_PROFILE_SCHEMA_VERSION and DM_PROFILE_LAYOUT_SIZE (required by profile_rt.c)
set(SCHEMA_H ${PROFILE_INC}/schema_stamp.h)
add_custom_command(OUTPUT ${SCHEMA_H}
  COMMAND ${Python3_EXECUTABLE} ${PROFILE_DIR}/gen_runtime_profile.py schema-stamp ${SCHEMA_H}
  DEPENDS ${PROFILE_DIR}/gen_runtime_profile.py ${ROOT}/dm/games/game.py)
set_source_files_properties(${PROFILE_DIR}/profile_rt.c PROPERTIES
  OBJECT_DEPENDS ${SCHEMA_H}
  COMPILE_OPTIONS -I${PROFILE_INC})

# --- The combined host+gfx library (dmhost) ---
add_library(dmhost STATIC ${HOST_C} ${GFX_C} ${ASM_OUT})
target_include_directories(dmhost PUBLIC
  ${PROFILE_INC}
  ${GFX}/nvn/include
  ${TP}/vulkan-headers/include   # in-tree Vulkan headers (vulkan_core.h + vulkan_wayland.h)
  ${TP}/libvpx ${TP}/libvpx/config
  ${TP}/dav1d/include
  ${TP}/stb_vorbis
  ${HOST}
)
target_compile_definitions(dmhost PUBLIC _GNU_SOURCE DM_NUP_ZSTD)
target_compile_options(dmhost PRIVATE -O1 -g -Wall -Wno-unused-function -Wno-format -fvisibility=hidden)
# Draw path at -O2 as in the Android build
file(GLOB VKA_C ${GFX}/vk_android/*.c)
set_source_files_properties(${VKA_C} PROPERTIES COMPILE_OPTIONS -O2)
target_link_libraries(dmhost PUBLIC vpx dav1d zstddec m dl z)
# Wayland client for the Vulkan surface extension (linked by callers that use it)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)
target_include_directories(dmhost PUBLIC ${WAYLAND_INCLUDE_DIRS})
target_link_libraries(dmhost PUBLIC ${WAYLAND_LIBRARIES})

# --- Stage 1 selftest launcher ---
# Opens a Wayland window via SDL2, sets it on the replay, and runs NXHOST_VK_SELFTEST frames.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  pkg_check_modules(SDL2 REQUIRED sdl2)
endif()
add_executable(selftest_launcher selftest_main.c)
target_include_directories(selftest_launcher PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(selftest_launcher PRIVATE dmhost ${SDL2_LIBRARIES})
target_compile_options(selftest_launcher PRIVATE -O1 -g -Wall -Wno-unused-function -Wno-format)

# --- dmhost-linux: full game launcher (Stage 3) ---
# dmhost_main.c: SDL2 window + input pump (defines main()).
# dm/host/main.c: host init / game dispatch, compiled as dm_host_main_impl() so it
# does not collide with dmhost_main.c's main(). front_linux.c's nxhost_main() calls it.
add_executable(dmhost-linux dmhost_main.c ${HOST}/main.c)
set_source_files_properties(${HOST}/main.c PROPERTIES
  COMPILE_DEFINITIONS "main=dm_host_main_impl")
target_include_directories(dmhost-linux PRIVATE ${SDL2_INCLUDE_DIRS} ${PROFILE_INC})
target_link_libraries(dmhost-linux PRIVATE dmhost ${SDL2_LIBRARIES})
target_compile_options(dmhost-linux PRIVATE -O1 -g -Wall -Wno-unused-function -Wno-format)
