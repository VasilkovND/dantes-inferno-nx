# dante - Dante's Inferno (Xbox 360, Title Update 2) on the Nintendo Switch.
#
# The recompiled game code is generated from your own default.xex + default.xexp by tools/codegen.sh (ReXGlue's
# codegen with Hell's Gate's manifest and fixes, https://github.com/florinp93/hells-gate-recomp), never committed.
# Point DANTE_CODEGEN_DIR at its output. At configure time tools/prepare_tree.py mirrors generated/default/ as
# symlinks with a Switch PCH (tools/make_nx_pch.py); see PORTING.md. tools/build_mac.sh does all of it.
#
# Configure (Switch):
#   cmake -S dante -B dante/out/sw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXSDK_DIR=$PWD/sdk -DDANTE_CODEGEN_DIR=$PWD/dante/out/codegen -DREXGLUE_SWITCH_DEKO3D=ON
#   cmake --build dante/out/sw

cmake_minimum_required(VERSION 3.25)
project(dante LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/dante_switch.cmake)

if(NOT REXGLUE_SWITCH_DEKO3D)
    message(FATAL_ERROR "dante: only the deko3d build exists (-DREXGLUE_SWITCH_DEKO3D=ON)")
endif()

add_executable(dante
    src/main.cpp
    src/dante_gpu.cpp
    src/dante_shader_dump.cpp
    src/dante_shaders.cpp
    src/dante_audio.cpp
    src/dante_cpu.cpp
    src/dante_video.cpp
    src/dante_skin.cpp
    src/dante_god.cpp
    # The shader library reader, shared with the PC tools (shaders/).
    ${CMAKE_CURRENT_SOURCE_DIR}/../shaders/ds_shader_library.cpp
)

dante_setup_target(dante)
rexglue_switch_add_deko3d(dante)
target_compile_definitions(dante PRIVATE DANTE_DEKO3D=1)
target_include_directories(dante PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shaders ${REXSDK_DIR}/thirdparty/xxHash
                           ${REXSDK_DIR}/thirdparty/FFmpeg)
# The VP6 decoder for the FMVs (src/dante_video.cpp, sdk-patches/0009).
target_link_libraries(dante PRIVATE libavcodec libavutil)

# The version (Homebrew Menu and the first lines of the log) and the commit it was built from, so a log a player sends
# says which build made it. The commit is read when CMake configures (build_mac.sh RECONFIGURE=1 refreshes it).
set(DANTE_VERSION "1.0.0 (TU2)")
execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE DANTE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git status --porcelain --untracked-files=no --ignore-submodules=all WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE DANTE_SUCIO OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT DANTE_COMMIT)
    set(DANTE_COMMIT "unknown")
elseif(DANTE_SUCIO)
    string(APPEND DANTE_COMMIT "+changes")
endif()
target_compile_definitions(dante PRIVATE DANTE_VERSION="${DANTE_VERSION}" DANTE_COMMIT="${DANTE_COMMIT}")

# Nintendo Switch: dante.nro. On the SD card: sdmc:/switch/dantes-inferno-nx/dantes-inferno-nx.nro, with game/ beside it
# (src/dante_app.h finds it there). No icon from the game: the Homebrew Menu shows the default one.
if(REXGLUE_PLATFORM_SWITCH)
    rexglue_switch_add_nro(dante
        NAME "Dante's Inferno"
        AUTHOR "VasilkovND"
        VERSION "${DANTE_VERSION}")
endif()
