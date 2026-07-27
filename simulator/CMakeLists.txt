cmake_minimum_required(VERSION 3.20)
project(busybar_simulator C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(
        CMAKE_BUILD_TYPE Debug
        CACHE STRING "Simulator build type"
        FORCE
    )
endif()

set(
    BUSYBAR_SIM_HEAP_MB
    16
    CACHE STRING
    "Host furi heap in MiB (minimum 8; target firmware has about 2.5 MiB total RAM)"
)
if(NOT BUSYBAR_SIM_HEAP_MB MATCHES "^[0-9]+$" OR BUSYBAR_SIM_HEAP_MB LESS 8)
    message(FATAL_ERROR "BUSYBAR_SIM_HEAP_MB must be a whole number of at least 8")
endif()
math(EXPR BUSYBAR_SIM_HEAP_SIZE_BYTES "${BUSYBAR_SIM_HEAP_MB} * 1024 * 1024")

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORELIBS ${REPO_ROOT}/fbt_layers/core_libs/lib)
set(FREERTOS ${REPO_ROOT}/fbt_layers/freertos/lib/FreeRTOS-Kernel)
set(POSIX_PORT ${FREERTOS}/portable/ThirdParty/GCC/Posix)
set(LVGL ${REPO_ROOT}/lib/lvgl)
set(SERVICES ${REPO_ROOT}/applications/services)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
if(NOT APPLE)
    find_path(DNSSD_INCLUDE_DIR dns_sd.h REQUIRED)
    find_library(DNSSD_LIBRARY dns_sd REQUIRED)
endif()

# uv runs the asset and source-patching tools without a firmware toolchain.
find_program(UV_EXECUTABLE uv REQUIRED)

# ---------------------------------------------------------------------------
# Staged core_libs tree
#
# furi's core/check.h passes the crash message through r12 with inline asm and
# core/check.c dumps Cortex-M registers. Both are expanded or linked into every
# translation unit, so the tree is copied and those two files swapped for host
# equivalents. Copying keeps the submodule pristine.
#
# The whole of core_libs is staged rather than furi alone: <datetime/...> and
# friends need their parent directory on the include path, and if that were
# the original tree it would also expose the unpatched furi, giving every type
# two conflicting definitions.
# ---------------------------------------------------------------------------
set(CORELIBS_STAGE ${CMAKE_BINARY_DIR}/core_libs_host)
set(FURI_STAGE ${CORELIBS_STAGE}/furi)

file(
    GLOB_RECURSE CORELIBS_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    ${CORELIBS}/*
)
set(CORELIBS_STAGE_STAMP ${CORELIBS_STAGE}/.stage-complete)
add_custom_command(
    OUTPUT ${CORELIBS_STAGE_STAMP}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CORELIBS_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CORELIBS} ${CORELIBS_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/shim/check_host.h
            ${FURI_STAGE}/core/check.h
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/shim/check_host.c
            ${FURI_STAGE}/core/check.c
    # flipper.c is the on-target firmware entry point and starts every service.
    COMMAND ${CMAKE_COMMAND} -E rm -f ${FURI_STAGE}/flipper.c
    COMMAND ${CMAKE_COMMAND} -E touch ${CORELIBS_STAGE_STAMP}
    DEPENDS
        ${CORELIBS_INPUTS}
        ${CMAKE_CURRENT_SOURCE_DIR}/shim/check_host.h
        ${CMAKE_CURRENT_SOURCE_DIR}/shim/check_host.c
    COMMENT "Staging host-compatible core libraries"
    VERBATIM
)
add_custom_target(simulator_core_stage DEPENDS ${CORELIBS_STAGE_STAMP})

file(
    GLOB FURI_ORIGINAL_SOURCES
    CONFIGURE_DEPENDS
    ${CORELIBS}/furi/core/*.c
    ${CORELIBS}/furi/furi.c
)
set(FURI_SOURCES "")
foreach(source ${FURI_ORIGINAL_SOURCES})
    file(RELATIVE_PATH relative_source ${CORELIBS} ${source})
    list(APPEND FURI_SOURCES ${CORELIBS_STAGE}/${relative_source})
endforeach()
set_source_files_properties(${FURI_SOURCES} PROPERTIES GENERATED TRUE)

# Firmware code must keep calling furi's malloc/free so heap pressure remains
# observable. On ELF hosts, however, exporting those executable symbols also
# interposes them into SDL and its dependencies. Keep this translation unit
# local so shared libraries continue to use the system allocator.
if(NOT APPLE)
    set_source_files_properties(
        ${FURI_STAGE}/core/memmgr.c
        PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden"
    )
endif()

set(MEMMGR_HEAP_PATCHED ${FURI_STAGE}/core/memmgr_heap_host.c)
add_custom_command(
    OUTPUT ${MEMMGR_HEAP_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_memmgr_heap.py
            --input ${CORELIBS}/furi/core/memmgr_heap.c
            --output ${MEMMGR_HEAP_PATCHED}
    DEPENDS
        ${CORELIBS_STAGE_STAMP}
        ${CORELIBS}/furi/core/memmgr_heap.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_memmgr_heap.py
    COMMENT "Patching the furi heap guard for POSIX threads"
    VERBATIM
)
list(REMOVE_ITEM FURI_SOURCES ${FURI_STAGE}/core/memmgr_heap.c)
list(APPEND FURI_SOURCES ${MEMMGR_HEAP_PATCHED})

# The About screen shows the firmware version. lib/version/version.c cannot be
# compiled here -- it static_asserts its struct layout against 32-bit offsets --
# so src/platform_services_host.c provides the accessors and reads this header,
# which mirrors what fbt generates. Dirty state has no complete static
# dependency, so the generator runs on every build and only replaces the file
# when its contents change.
set(VERSION_HEADER ${CORELIBS_STAGE}/version/version.inc.h)
add_custom_target(
    simulator_version_header ALL
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_version_header.py
            --repo ${REPO_ROOT}
            --output ${VERSION_HEADER}
    BYPRODUCTS ${VERSION_HEADER}
    COMMENT "Refreshing simulator version metadata"
    VERBATIM
)
add_dependencies(simulator_version_header simulator_core_stage)

# ---------------------------------------------------------------------------
# FreeRTOS with the POSIX simulator port
# ---------------------------------------------------------------------------
set(FREERTOS_SOURCES
    ${FREERTOS}/tasks.c
    ${FREERTOS}/queue.c
    ${FREERTOS}/list.c
    ${FREERTOS}/timers.c
    ${FREERTOS}/event_groups.c
    ${FREERTOS}/stream_buffer.c
    # No heap_N.c here: furi's core/memmgr_heap.c is the allocator, running
    # over the static region furi_hal_host.c hands it. heap_3 would forward to
    # malloc(), which furi overrides to call back into pvPortMalloc.
    ${POSIX_PORT}/utils/wait_for_event.c
)

# The V10.5.1 port raises the tick with setitimer(), i.e. at the process, so
# any thread that has not blocked SIGALRM can end up running the scheduler.
# SDL brings AppKit threads with it and one of them caught the tick and wedged
# the whole kernel. Upstream aims the signal at the current task's thread
# instead; this backports that.
set(POSIX_PORT_PATCHED ${CMAKE_BINARY_DIR}/freertos_host/port.c)

add_custom_command(
    OUTPUT ${POSIX_PORT_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_posix_port.py
            --input ${POSIX_PORT}/port.c
            --output ${POSIX_PORT_PATCHED}
    DEPENDS
        ${POSIX_PORT}/port.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_posix_port.py
    COMMENT "Patching the FreeRTOS POSIX port"
    VERBATIM
)

list(APPEND FREERTOS_SOURCES ${POSIX_PORT_PATCHED})

# ---------------------------------------------------------------------------
# LVGL — same source set as lib/lvgl.scons, plus the SDL backend the firmware
# build has no use for.
# ---------------------------------------------------------------------------
file(GLOB LVGL_SOURCES CONFIGURE_DEPENDS
    ${LVGL}/src/*.c
    ${LVGL}/src/core/*.c
    ${LVGL}/src/display/*.c
    ${LVGL}/src/draw/*.c
    ${LVGL}/src/draw/sw/*.c
    ${LVGL}/src/draw/sw/blend/*.c
    ${LVGL}/src/font/*.c
    ${LVGL}/src/indev/*.c
    ${LVGL}/src/layouts/*.c
    ${LVGL}/src/layouts/*/*.c
    ${LVGL}/src/misc/*.c
    ${LVGL}/src/misc/cache/*.c
    ${LVGL}/src/stdlib/*.c
    ${LVGL}/src/stdlib/clib/*.c
    ${LVGL}/src/osal/*.c
    ${LVGL}/src/others/sysmon/*.c
    ${LVGL}/src/others/observer/*.c
    ${LVGL}/src/themes/*.c
    ${LVGL}/src/themes/*/*.c
    ${LVGL}/src/tick/*.c
    ${LVGL}/src/widgets/*/*.c
    ${LVGL}/src/libs/lodepng/*.c
    ${LVGL}/src/libs/qrcode/*.c
)

# lvgl_addons/fs is deliberately absent: it routes LVGL file access through
# the storage service, which the simulator replaces with src/lv_fs_host.c.
# fbt rewrites the binary image decoder so it claims the firmware's .image
# extension instead of .bin; without it every icon fails to decode.
set(BIN_DECODER_PATCHED ${CMAKE_BINARY_DIR}/lvgl_host/lv_bin_decoder.c)
set(LODEPNG_PATCHED ${CMAKE_BINARY_DIR}/lvgl_host/lodepng.c)

add_custom_command(
    OUTPUT ${BIN_DECODER_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_bin_decoder.py
            --input ${LVGL}/src/libs/bin_decoder/lv_bin_decoder.c
            --output ${BIN_DECODER_PATCHED}
    DEPENDS
        ${LVGL}/src/libs/bin_decoder/lv_bin_decoder.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_bin_decoder.py
    COMMENT "Patching the LVGL binary image decoder"
    VERBATIM
)

list(APPEND LVGL_SOURCES ${BIN_DECODER_PATCHED})

add_custom_command(
    OUTPUT ${LODEPNG_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_lodepng_allocator.py
            --input ${LVGL}/src/libs/lodepng/lodepng.c
            --output ${LODEPNG_PATCHED}
    DEPENDS
        ${LVGL}/src/libs/lodepng/lodepng.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_lodepng_allocator.py
    COMMENT "Separating simulator PNG workspace from the furi heap"
    VERBATIM
)
list(REMOVE_ITEM LVGL_SOURCES ${LVGL}/src/libs/lodepng/lodepng.c)
list(APPEND LVGL_SOURCES ${LODEPNG_PATCHED})

file(GLOB LVGL_ADDON_SOURCES CONFIGURE_DEPENDS
    ${REPO_ROOT}/lib/lvgl_addons/extensions/*.c
    ${REPO_ROOT}/lib/lvgl_addons/fonts/*.c
    ${REPO_ROOT}/lib/lvgl_addons/osal/*.c
    ${REPO_ROOT}/lib/lvgl_addons/themes/*.c
)

# ---------------------------------------------------------------------------
# Internal image assets
#
# gui/modules/image.c includes the generated assets_images.h. Under fbt that
# comes from lib/internal_assets.scons; here the same conversion runs through
# uv so the simulator needs no firmware toolchain.
# ---------------------------------------------------------------------------
set(ASSETS_GEN_DIR ${CMAKE_BINARY_DIR}/assets_internal)

file(
    GLOB INTERNAL_ASSET_INPUTS
    CONFIGURE_DEPENDS
    ${REPO_ROOT}/assets/images/internal/*.png
)
file(
    GLOB LVGL_IMAGE_TOOL_INPUTS
    CONFIGURE_DEPENDS
    ${REPO_ROOT}/lib/lvgl/scripts/*.py
)
set(ASSET_SOURCES "")
foreach(source ${INTERNAL_ASSET_INPUTS})
    get_filename_component(asset_name ${source} NAME_WE)
    list(APPEND ASSET_SOURCES ${ASSETS_GEN_DIR}/I_${asset_name}.c)
endforeach()
set(INTERNAL_ASSET_HEADER ${ASSETS_GEN_DIR}/assets_images.h)
add_custom_command(
    OUTPUT ${INTERNAL_ASSET_HEADER} ${ASSET_SOURCES}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_internal_assets.py
            --images ${REPO_ROOT}/assets/images/internal
            --out ${ASSETS_GEN_DIR}
    DEPENDS
        ${INTERNAL_ASSET_INPUTS}
        ${LVGL_IMAGE_TOOL_INPUTS}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_internal_assets.py
    COMMENT "Generating internal LVGL images"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Asset root
#
# The UI asks for paths like /ext/apps_assets/shared/fonts/busy_regular_7.font
# and /ext/apps_assets/clock/images/calendar_13x14.image. Fonts ship ready to
# use, but images and animations are build products, so the same conversions
# fbt performs run here into a tree with the device's layout.
# ---------------------------------------------------------------------------
set(ASSETS_ROOT ${CMAKE_BINARY_DIR}/assets_root)

file(
    GLOB_RECURSE RUNTIME_ASSET_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    ${REPO_ROOT}/assets/animations/*
    ${REPO_ROOT}/assets/images/external/*
    ${REPO_ROOT}/assets/shared/animations/*
    ${REPO_ROOT}/assets/shared/fonts/*
    ${REPO_ROOT}/assets/shared/images/external/*
    ${REPO_ROOT}/assets/shared/sounds/*
    ${REPO_ROOT}/assets/sounds/*
    ${REPO_ROOT}/applications/*/*/resources/*
)
set(RUNTIME_ASSET_STAMP ${ASSETS_ROOT}/.generation-complete)
add_custom_command(
    OUTPUT ${RUNTIME_ASSET_STAMP}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_runtime_assets.py
            --root ${ASSETS_ROOT}
    COMMAND ${CMAKE_COMMAND} -E touch ${RUNTIME_ASSET_STAMP}
    DEPENDS
        ${RUNTIME_ASSET_INPUTS}
        ${LVGL_IMAGE_TOOL_INPUTS}
        ${REPO_ROOT}/scripts/seq2anim.py
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_runtime_assets.py
    COMMENT "Generating simulator runtime assets"
    VERBATIM
)
add_custom_target(simulator_runtime_assets DEPENDS ${RUNTIME_ASSET_STAMP})

# ---------------------------------------------------------------------------
# Device renders
#
# The window draws the two virtual displays onto renders of the device. They
# are stripped to raw frames here so src/sim_background.c can read them without
# a PNG decoder — see tools/gen_panel_backgrounds.py for why that matters.
# ---------------------------------------------------------------------------
set(BACKGROUNDS_GEN_DIR ${CMAKE_BINARY_DIR}/backgrounds)

set(BACKGROUND_OUTPUTS
    ${BACKGROUNDS_GEN_DIR}/front.rgb24
    ${BACKGROUNDS_GEN_DIR}/back.rgb24
    ${BACKGROUNDS_GEN_DIR}/sim_background_generated.h
)
add_custom_command(
    OUTPUT ${BACKGROUND_OUTPUTS}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_panel_backgrounds.py
            --assets ${CMAKE_CURRENT_SOURCE_DIR}/assets
            --out ${BACKGROUNDS_GEN_DIR}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/front.png
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/back.png
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_panel_backgrounds.py
    COMMENT "Generating simulator device backgrounds"
    VERBATIM
)
add_custom_target(simulator_backgrounds DEPENDS ${BACKGROUND_OUTPUTS})

# ---------------------------------------------------------------------------
# Firmware UI layer
# ---------------------------------------------------------------------------
file(GLOB GUI_SOURCES CONFIGURE_DEPENDS
    ${SERVICES}/gui/*.c
    ${SERVICES}/gui/modules/*.c
    ${SERVICES}/gui/modules/private/*.c
    ${SERVICES}/font_registry/font_registry.c
)

# Support libraries the widget layer links against: animation playback, and
# the colour and timer helpers out of toolbox.
#
# anim_file.c needs one GCC nested function rewritten before clang will take
# it, so it is compiled from a patched copy in the build directory.
set(ANIM_FILE_PATCHED ${CMAKE_BINARY_DIR}/anim_file_host/anim_file.c)

add_custom_command(
    OUTPUT ${ANIM_FILE_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_anim_file.py
            --input ${REPO_ROOT}/lib/anim_file/anim_file.c
            --output ${ANIM_FILE_PATCHED}
    DEPENDS
        ${REPO_ROOT}/lib/anim_file/anim_file.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_anim_file.py
    COMMENT "Patching animation file support for the host"
    VERBATIM
)

# anim_file_seq.c compiles as it stands, but walks a looping single-frame
# animation past the end of its file, which the busy timer's progress bar is.
# The patched copy stops the log filling with "Load error" once a second.
set(ANIM_FILE_SEQ_PATCHED ${CMAKE_BINARY_DIR}/anim_file_host/anim_file_seq.c)

add_custom_command(
    OUTPUT ${ANIM_FILE_SEQ_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_anim_file_seq.py
            --input ${REPO_ROOT}/lib/anim_file/components/anim_file_seq.c
            --output ${ANIM_FILE_SEQ_PATCHED}
    DEPENDS
        ${REPO_ROOT}/lib/anim_file/components/anim_file_seq.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_anim_file_seq.py
    COMMENT "Patching animation sequence support for the host"
    VERBATIM
)

file(GLOB ANIM_FILE_SOURCES CONFIGURE_DEPENDS ${REPO_ROOT}/lib/anim_file/components/*.c)
list(REMOVE_ITEM ANIM_FILE_SOURCES ${REPO_ROOT}/lib/anim_file/components/anim_file_seq.c)
list(APPEND ANIM_FILE_SOURCES ${ANIM_FILE_PATCHED} ${ANIM_FILE_SEQ_PATCHED})

# dsp.c convolves with hand-written Thumb-2 DSP instructions; the host build
# uses a copy with that one helper expressed in portable C.
set(DSP_PATCHED ${CMAKE_BINARY_DIR}/toolbox_host/dsp.c)

add_custom_command(
    OUTPUT ${DSP_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_dsp.py
            --input ${REPO_ROOT}/lib/toolbox/dsp.c
            --output ${DSP_PATCHED}
    DEPENDS
        ${REPO_ROOT}/lib/toolbox/dsp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_dsp.py
    COMMENT "Patching DSP helpers for the host"
    VERBATIM
)

set(TOOLBOX_SOURCES
    ${REPO_ROOT}/lib/toolbox/color.c
    ${REPO_ROOT}/lib/toolbox/timers.c
    ${DSP_PATCHED}
    ${REPO_ROOT}/lib/toolbox/rle_encode.c
    # The state stream stamps each screen frame with a checksum so a client can
    # skip one it already has.
    ${REPO_ROOT}/lib/toolbox/crc32_calc.c
    ${REPO_ROOT}/lib/toolbox/strint.c
    ${REPO_ROOT}/lib/toolbox/path.c
    ${REPO_ROOT}/lib/toolbox/value_index.c
    ${REPO_ROOT}/lib/storage_utils/temp_file.c
    ${REPO_ROOT}/lib/stb/stb.c
    # Predicates over FileInfo, shared by the storage service and its callers.
    ${SERVICES}/storage/filesystem_api.c
    # Address formatting for /api/wifi; pure string work over the status the
    # stub Wi-Fi service reports.
    ${SERVICES}/wifi/wifi_util.c
    # The MQTT broker settings are stored and parsed as JSON on the device too,
    # so /api/account round-trips through the real serialiser.
    ${SERVICES}/mqtt/mqtt_config.c
)

# ---------------------------------------------------------------------------
# HTTP API
#
# The device's REST API is what busylib and the mobile app talk to, so the real
# web_server service and its handlers are built here rather than reimplemented.
# mongoose needs no porting: it detects the host as MG_ARCH_UNIX and uses
# ordinary BSD sockets, where on the device it runs over lwIP.
#
# Host-only source changes keep the listen address configurable and make the
# status handler's fixed-width integers portable to LP64 -- see the patch tools.
# ---------------------------------------------------------------------------
set(MONGOOSE ${REPO_ROOT}/lib/mongoose)

if(NOT EXISTS ${MONGOOSE}/mongoose.c)
    message(FATAL_ERROR
        "lib/mongoose is empty. The HTTP API needs it:\n"
        "    git submodule update --init lib/mongoose")
endif()

set(WEB_SERVER_PATCHED ${CMAKE_BINARY_DIR}/web_server_host/web_server.c)
set(API_STATUS_PATCHED ${CMAKE_BINARY_DIR}/web_server_host/api_status.c)

add_custom_command(
    OUTPUT ${WEB_SERVER_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_web_server.py
            --input ${SERVICES}/web_server/web_server.c
            --output ${WEB_SERVER_PATCHED}
    DEPENDS
        ${SERVICES}/web_server/web_server.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_web_server.py
    COMMENT "Patching the web server listen address"
    VERBATIM
)

add_custom_command(
    OUTPUT ${API_STATUS_PATCHED}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_api_status.py
            --input ${SERVICES}/web_server/http_api/api_status.c
            --output ${API_STATUS_PATCHED}
    DEPENDS
        ${SERVICES}/web_server/http_api/api_status.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_api_status.py
    COMMENT "Patching fixed-width status API formatting for the host"
    VERBATIM
)

# ---------------------------------------------------------------------------
# The state stream
#
# /api/status/ws is a WebSocket carrying protobuf, so the simulator needs the
# same three pieces the device has: the message definitions turned into C by
# nanopb's generator, nanopb's encoder, and the state_publisher service that
# watches every other service and serialises what changed.
#
# Nothing here is reimplemented -- a client decoding the simulator's frames is
# decoding the device's own schema, from the same .proto files.
# ---------------------------------------------------------------------------
set(NANOPB ${REPO_ROOT}/lib/nanopb)
set(PROTO_SRC ${REPO_ROOT}/assets/proto)

foreach(submodule ${NANOPB}/pb.h ${PROTO_SRC}/state.proto)
    if(NOT EXISTS ${submodule})
        message(FATAL_ERROR
            "${submodule} is missing. The state stream needs it:\n"
            "    git submodule update --init assets/proto lib/nanopb")
    endif()
endforeach()

set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/proto)

file(
    GLOB_RECURSE PROTO_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    ${PROTO_SRC}/*.proto
)
file(
    GLOB_RECURSE NANOPB_GENERATOR_INPUTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    ${NANOPB}/generator/*.py
    ${NANOPB}/generator/*.proto
)
set(PROTO_SOURCES "")
set(PROTO_HEADERS "")
foreach(source ${PROTO_INPUTS})
    file(RELATIVE_PATH relative_proto ${PROTO_SRC} ${source})
    string(REGEX REPLACE "\\.proto$" ".pb.c" relative_c ${relative_proto})
    string(REGEX REPLACE "\\.proto$" ".pb.h" relative_h ${relative_proto})
    list(APPEND PROTO_SOURCES ${PROTO_GEN_DIR}/${relative_c})
    list(APPEND PROTO_HEADERS ${PROTO_GEN_DIR}/${relative_h})
endforeach()
add_custom_command(
    OUTPUT ${PROTO_SOURCES} ${PROTO_HEADERS}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_proto.py
            --proto ${PROTO_SRC}
            --out ${PROTO_GEN_DIR}
    DEPENDS
        ${PROTO_INPUTS}
        ${NANOPB_GENERATOR_INPUTS}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_proto.py
    COMMENT "Generating nanopb state-stream sources"
    VERBATIM
)

list(APPEND PROTO_SOURCES
    ${NANOPB}/pb_encode.c
    ${NANOPB}/pb_decode.c
    ${NANOPB}/pb_common.c
)

file(GLOB STATE_PUBLISHER_SOURCES CONFIGURE_DEPENDS ${SERVICES}/state_publisher/*.c)

set(WEB_SERVER_SOURCES
    ${WEB_SERVER_PATCHED}
    ${SERVICES}/web_server/http_api/api_account.c
    ${SERVICES}/web_server/http_api/api_assets.c
    ${SERVICES}/web_server/http_api/api_audio.c
    ${SERVICES}/web_server/http_api/api_ble.c
    ${SERVICES}/web_server/http_api/api_busy.c
    ${SERVICES}/web_server/http_api/api_display.c
    ${SERVICES}/web_server/http_api/api_input.c
    ${SERVICES}/web_server/http_api/api_log.c
    ${SERVICES}/web_server/http_api/api_name.c
    ${SERVICES}/web_server/http_api/api_root.c
    ${SERVICES}/web_server/http_api/api_smart_home.c
    ${API_STATUS_PATCHED}
    ${SERVICES}/web_server/http_api/api_status_streaming.c
    ${SERVICES}/web_server/http_api/api_storage.c
    ${SERVICES}/web_server/http_api/api_streaming.c
    ${SERVICES}/web_server/http_api/api_time.c
    ${SERVICES}/web_server/http_api/api_update.c
    ${SERVICES}/web_server/http_api/api_wifi.c
    ${MONGOOSE}/mongoose.c
    # The device's glue, unchanged: its file layer is written against the
    # storage service, which src/storage_host.c implements.
    ${REPO_ROOT}/targets/f21/mongoose/mongoose_glue.c
)

# Two more services the API needs that are the real thing here: canvas, which
# is what /api/display/draw draws through and needs only gui and the loader,
# and log_storage, a ring buffer behind a furi log handler backing
# /api/log_dump.
file(GLOB API_SERVICE_SOURCES CONFIGURE_DEPENDS
    ${SERVICES}/canvas/*.c
    ${SERVICES}/canvas/widgets/*.c
)

list(APPEND API_SERVICE_SOURCES ${SERVICES}/log_storage/log_storage.c)

# Applications the simulator can launch. Only apps whose service dependencies
# the simulator can satisfy are listed; see src/services_host.c.
file(
    GLOB SETTING_PROVIDER_TYPE_SOURCES
    CONFIGURE_DEPENDS
    ${REPO_ROOT}/lib/setting_provider/types/*.c
)

# Settings apps describe themselves through this; the settings menu builds the
# list by calling each app's entry point with a descriptor.
file(GLOB SETTINGS_HELPERS_SOURCES CONFIGURE_DEPENDS ${REPO_ROOT}/lib/settings_helpers/*.c)

# Bits of lib/toolbox that apps need. The directory itself cannot go on the
# include path: its timers.h would shadow FreeRTOS's.
# Apps include these as <name.h>, so each gets a forwarding header in a
# directory that is on the path. Forwarding rather than copying: other headers
# reach the same files as <toolbox/name.h>, and two copies at two paths would
# be two sets of typedefs.
foreach(header tzutil.h api_lock.h json_helper.h color.h)
    file(WRITE ${CMAKE_BINARY_DIR}/toolbox_host/${header}
         "#pragma once\n#include <toolbox/${header}>\n")
endforeach()

set(TZ_SOURCES
    ${REPO_ROOT}/lib/toolbox/tzutil.c
    ${REPO_ROOT}/lib/toolbox/json_helper.c
)
file(GLOB UTZ_ORIGINAL_SOURCES CONFIGURE_DEPENDS ${CORELIBS}/utz/*.c)
set(UTZ_STAGE_SOURCES "")
foreach(source ${UTZ_ORIGINAL_SOURCES})
    get_filename_component(source_name ${source} NAME)
    list(APPEND UTZ_STAGE_SOURCES ${CORELIBS_STAGE}/utz/${source_name})
endforeach()
list(APPEND TZ_SOURCES ${UTZ_STAGE_SOURCES})
set_source_files_properties(
    ${UTZ_STAGE_SOURCES}
    ${CORELIBS_STAGE}/datetime/datetime.c
    PROPERTIES GENERATED TRUE
)

# Apps linked into the simulator, as paths under applications/. Each one's
# sources are globbed and its application.fam feeds the generated registry, so
# adding an app is one line here.
set(SIM_APP_DIRS
    main/busy
    main/clock
    main/power_on
    main/soft_off
    main/settings_menu
    system/apps_menu
    system/message
    settings/about
    settings/system_settings
    settings/debug_app_list
    settings/time_settings
    settings/brightness_settings
    settings/sound_settings
    settings/wifi_settings
    settings/ble_settings
    settings/matter_settings
    settings/account_settings
    settings/firmware
    # Debug apps, minus the ones the host cannot take: crash_test faults on
    # purpose, front_display_test and the sensor and LED ones reach for GPIO,
    # storage_bench and unit_tests are CLI, and anim_test uses a GCC nested
    # function that clang rejects.
    debug/gui_test
    debug/input_test
    debug/back_display_test
    debug/dummy
)

# The busy timer is a real service, not a stub: it needs only the RTC and
# records the simulator already provides, so the countdown behaves as it does
# on the device.
file(GLOB BUSY_TIMER_SOURCES CONFIGURE_DEPENDS
    ${SERVICES}/busy_timer/*.c
    ${SERVICES}/busy_timer/settings/*.c
)

# Brightness is also the firmware service, not a static host value. It persists
# through the writable overlay, publishes the same FuriState as the device, and
# drives the host display's brightness/contrast simulation.
file(GLOB BRIGHTNESS_CONTROL_SOURCES CONFIGURE_DEPENDS
    ${SERVICES}/brightness_control/*.c
    ${SERVICES}/brightness_control/settings/*.c
)

# The network synchronization loop is deliberately absent, but its settings
# codec is useful on the host: timezone and 12/24-hour choices should survive a
# restart just as they do on the device.
file(GLOB TIME_SETTINGS_SOURCES CONFIGURE_DEPENDS
    ${SERVICES}/time/settings/*.c
)

# The mode selector, and what makes it mean anything.
#
# The five mode keys are positions of a rotary switch, not buttons: the input
# service records which one is selected and the desktop service watches that,
# stopping the running app and asking the loader for the one the position maps
# to. Both are the firmware's own, so the transitions, the wipe animation and
# the app-exit handshake are the device's rather than an imitation.
#
# loader_cli.c is left out: it registers a CLI command, and there is no CLI.
set(DESKTOP_SOURCES
    ${SERVICES}/desktop/desktop.c
    ${SERVICES}/desktop/desktop_overlay.c
    ${SERVICES}/desktop/desktop_start_request.c
    ${SERVICES}/loader/loader.c
)

set(APP_SOURCES "")
set(APP_MANIFESTS "")
foreach(app_dir ${SIM_APP_DIRS})
    file(GLOB app_sources CONFIGURE_DEPENDS
        ${REPO_ROOT}/applications/${app_dir}/*.c
        ${REPO_ROOT}/applications/${app_dir}/scenes/*.c
        ${REPO_ROOT}/applications/${app_dir}/settings/*.c
        ${REPO_ROOT}/applications/${app_dir}/helpers/*.c
        ${REPO_ROOT}/applications/${app_dir}/models/*.c
        ${REPO_ROOT}/applications/${app_dir}/views/*.c
        ${REPO_ROOT}/applications/${app_dir}/widgets/*.c
    )
    if(NOT app_sources)
        message(FATAL_ERROR "no sources under applications/${app_dir}")
    endif()
    list(APPEND APP_SOURCES ${app_sources})
    list(APPEND APP_MANIFESTS ${REPO_ROOT}/applications/${app_dir}/application.fam)
endforeach()

# fbt generates applications.c from the manifests; apps read the tables in it
# at runtime, so the host build needs its own copy listing what it links.
set(APP_REGISTRY ${CMAKE_BINARY_DIR}/apps_host/applications.c)

add_custom_command(
    OUTPUT ${APP_REGISTRY}
    COMMAND ${UV_EXECUTABLE} run --quiet
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_app_registry.py
            --output ${APP_REGISTRY}
            ${APP_MANIFESTS}
    DEPENDS
        ${APP_MANIFESTS}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_app_registry.py
    COMMENT "Generating the simulator app registry"
    VERBATIM
)

list(APPEND APP_SOURCES ${APP_REGISTRY})

file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim/*.c)
# check_host.c is compiled from its staged location as furi/core/check.c, where
# its relative includes resolve; building it here as well would duplicate every
# symbol it defines.
list(REMOVE_ITEM SHIM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/shim/check_host.c)
file(GLOB SIM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_executable(busybar-sim
    ${FURI_SOURCES}
    ${FREERTOS_SOURCES}
    ${LVGL_SOURCES}
    ${LVGL_ADDON_SOURCES}
    ${GUI_SOURCES}
    ${APP_SOURCES}
    ${SETTING_PROVIDER_TYPE_SOURCES}
    ${SETTINGS_HELPERS_SOURCES}
    ${BUSY_TIMER_SOURCES}
    ${BRIGHTNESS_CONTROL_SOURCES}
    ${TIME_SETTINGS_SOURCES}
    ${DESKTOP_SOURCES}
    ${WEB_SERVER_SOURCES}
    ${PROTO_SOURCES}
    ${STATE_PUBLISHER_SOURCES}
    ${API_SERVICE_SOURCES}
    ${TZ_SOURCES}
    ${ANIM_FILE_SOURCES}
    ${TOOLBOX_SOURCES}
    ${SHIM_SOURCES}
    ${SIM_SOURCES}
    ${ASSET_SOURCES}
    ${INTERNAL_ASSET_HEADER}
    ${PROTO_HEADERS}
    ${BACKGROUND_OUTPUTS}
    ${VERSION_HEADER}
    ${CORELIBS_STAGE}/datetime/datetime.c
    ${REPO_ROOT}/lib/setting_provider/setting_provider.c
    ${REPO_ROOT}/lib/toolbox/hex.c
    ${REPO_ROOT}/lib/cjson/cJSON.c
    ${REPO_ROOT}/lib/cli/args.c
)

add_dependencies(
    busybar-sim
    simulator_core_stage
    simulator_version_header
    simulator_runtime_assets
    simulator_backgrounds
)

target_include_directories(busybar-sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FURI_STAGE}
    ${CORELIBS_STAGE}
    ${CORELIBS_STAGE}/mlib
    ${REPO_ROOT}/lib
    ${REPO_ROOT}/lib/anim_file
    ${REPO_ROOT}/lib/setting_provider
    ${REPO_ROOT}/lib/cjson
    ${MONGOOSE}
    ${REPO_ROOT}/targets/f21/mongoose
    # web_server.c is compiled from a patched copy elsewhere; its "web_server_i.h"
    # and "http_api/..." includes resolve back to the original directory.
    # api_status.c is also staged, so preserve its source-directory include path.
    ${SERVICES}/web_server
    ${SERVICES}/web_server/http_api
    # State stream messages, as <state.pb.h> and <error.pb.h>. The generated
    # headers include "pb.h" unqualified, so nanopb's own directory goes on the
    # path as well; firmware sources reach the same headers as <nanopb/pb.h>,
    # which resolves under lib/ above.
    ${PROTO_GEN_DIR}
    ${NANOPB}
    # api_root.c and friends include <version.h>, not <version/version.h>.
    ${CORELIBS_STAGE}/version
    ${CORELIBS_STAGE}/datetime
    ${REPO_ROOT}/fbt_layers/freertos/lib
    ${REPO_ROOT}/fbt_layers/freertos/lib/FreeRTOS-glue
    ${FREERTOS}/include
    ${POSIX_PORT}
    ${POSIX_PORT}/utils
    ${LVGL}
    ${LVGL}/src
    ${SERVICES}
    ${REPO_ROOT}/applications
    ${REPO_ROOT}/applications/system
    # busy_i.h includes <applications/system/updater/updater.h>.
    ${REPO_ROOT}
    # applications.h, whose tables the generated registry above fills in.
    ${REPO_ROOT}/lib/firmware_applications_u5
    # Real HAL headers for the device; shim/furi_hal_host.c implements the
    # handful of entry points the UI actually calls.
    ${REPO_ROOT}/targets/f21/furi_hal
    # Last, so it only supplies headers for platform features that have no
    # implementation here at all.
    ${CMAKE_BINARY_DIR}/toolbox_host
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/platform
    ${REPO_ROOT}/applications/main
    ${ASSETS_GEN_DIR}
    ${BACKGROUNDS_GEN_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${DNSSD_INCLUDE_DIR}
)

target_compile_definitions(busybar-sim PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    FURI_DEBUG
    # Makes gui.c subscribe to the input pubsub that src/input_host.c creates.
    SRV_INPUT
    # Keeps automatic brightness enabled in the real brightness service; the
    # host light sensor publishes the same max-level event as device hardware.
    SRV_LIGHT_SENSOR
    BUSYBAR_SIM_HEAP_SIZE=${BUSYBAR_SIM_HEAP_SIZE_BYTES}
    BUSYBAR_SIM_DEFAULT_ASSETS="${ASSETS_ROOT}"
    # web_server_i.h asserts that its ConnectionContext fits mongoose's per
    # connection scratch space. The device's mongoose_config.h says 32 bytes,
    # which is five pointers there; here a pointer is eight.
    MG_DATA_SIZE=\(5*sizeof\(void*\)\)
    # Route mongoose's clock, log and entropy through furi, as on the device.
    # The device's mongoose_config.h is not read here: mongoose only includes it
    # when it cannot identify the architecture, and it identifies the host.
    MG_LOG_LEVEL=MG_LL_INFO
    MG_ENABLE_CUSTOM_MILLIS=1
    MG_ENABLE_CUSTOM_LOG=1
    MG_ENABLE_CUSTOM_RANDOM=1
    MG_ENABLE_DIRLIST=1
    # Files are served through the storage service by the device's glue, not
    # through mongoose's own POSIX layer.
    MG_ENABLE_POSIX_FS=0
    MG_TLS=MG_TLS_NONE
)

set(SIMULATOR_WARNING_OPTIONS -Wall -Wno-unused-parameter)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND SIMULATOR_WARNING_OPTIONS -Wno-tautological-constant-out-of-range-compare)
endif()

target_compile_options(busybar-sim PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/host_prelude.h
    ${SIMULATOR_WARNING_OPTIONS}
    ${SDL2_CFLAGS_OTHER}
)

target_link_libraries(busybar-sim PRIVATE ${SDL2_LIBRARIES} ${DNSSD_LIBRARY} pthread m)
target_link_directories(busybar-sim PRIVATE ${SDL2_LIBRARY_DIRS})

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME simulator.smoke
        COMMAND ${UV_EXECUTABLE} run --quiet
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/smoke_test.py
                --binary $<TARGET_FILE:busybar-sim>
                --assets ${ASSETS_ROOT}
    )
    set_tests_properties(simulator.smoke PROPERTIES TIMEOUT 60)
endif()
