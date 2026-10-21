# dante_switch.cmake - what the codegen-emitted rexglue.cmake gives a title, for the Switch build.
#
#   DANTE_CODEGEN_DIR   tools/codegen.sh's output (holds generated/default/, made from your default.xex + default.xexp)
#   REXSDK_DIR          this repository's sdk/ (rexglue-sdk-nx)
#
# The generated tree is used as it is, with one exception: its PCH. Its files include "dantes_inferno_pch.h" with
# quotes, which finds the copy next to them first, so the build compiles a mirror of symlinks (tools/prepare_tree.py)
# whose PCH is the Switch one (tools/make_nx_pch.py).

set(DANTE_CODEGEN_DIR "" CACHE PATH "tools/codegen.sh output folder (holds generated/default/)")
set(DANTE_GENERATED "${DANTE_CODEGEN_DIR}/generated/default")
if(NOT EXISTS "${DANTE_GENERATED}/sources.cmake")
    message(FATAL_ERROR
        "DANTE_CODEGEN_DIR must point at tools/codegen.sh's output "
        "(no generated/default/sources.cmake under '${DANTE_CODEGEN_DIR}')")
endif()

set(REXSDK_DIR "" CACHE PATH "Path to the rexglue-sdk(-nx) source tree")
if(NOT REXSDK_DIR)
    get_filename_component(REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../sdk" ABSOLUTE)
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
message(STATUS "dante: ReXGlue SDK from ${REXSDK_DIR}")

# The mirror of the generated tree with the Switch PCH.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
set(DANTE_TREE "${CMAKE_BINARY_DIR}/generated_nx")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_tree.py"
            "${DANTE_GENERATED}" "${DANTE_TREE}"
    RESULT_VARIABLE _dante_tree_rc)
if(NOT _dante_tree_rc EQUAL 0)
    message(FATAL_ERROR "dante: tools/prepare_tree.py failed (${_dante_tree_rc})")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${DANTE_GENERATED}/dantes_inferno_pch.h"
    "${DANTE_GENERATED}/sources.cmake")

set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING
    "Debug info level for generated code: line-tables-only, full, or none")
set(REXGLUE_RECOMP_OPTIONS "")
if(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "none")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-g0>)
elseif(REXGLUE_RECOMP_DEBUG_INFO STREQUAL "line-tables-only")
    list(APPEND REXGLUE_RECOMP_OPTIONS $<$<CXX_COMPILER_ID:Clang,AppleClang>:-gline-tables-only>)
endif()

# generated/default/sources.cmake: the support units and the recompiled ones. CMAKE_CURRENT_LIST_DIR there is the mirror.
include("${DANTE_TREE}/sources.cmake")
set(DANTE_GUEST_SOURCES ${GENERATED_SOURCES})
set_source_files_properties(${DANTE_GUEST_SOURCES} PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
# The registration unit is one function of ~37,000 SetFunction calls, run once at startup: at -O3 GCC spends ~6 minutes
# on it alone, on one core, longer than all the other units together on ten. -O1 takes seconds. It needs nothing from
# the PCH (its own includes), and a PCH built at -O3 cannot be used at -O1.
set_source_files_properties("${DANTE_TREE}/dantes_inferno_register.cpp" PROPERTIES
    COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS};-O1"
    SKIP_PRECOMPILE_HEADERS ON)
list(LENGTH DANTE_GUEST_SOURCES _dante_n)
message(STATUS "dante: ${_dante_n} recompiled sources from ${DANTE_GENERATED}")

# Usage: dante_setup_target(<target>)
macro(dante_setup_target target_name)
    add_library(${target_name}_recomp OBJECT ${DANTE_GUEST_SOURCES})
    # src/ for dantes_inferno_hooks.h, which the PCH includes.
    target_include_directories(${target_name}_recomp PRIVATE "${DANTE_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name}_recomp PRIVATE rex::runtime)
    rexglue_apply_target_settings(${target_name}_recomp)
    target_precompile_headers(${target_name}_recomp PRIVATE "${DANTE_TREE}/dantes_inferno_pch.h")
    target_compile_options(${target_name}_recomp PRIVATE ${REXGLUE_RECOMP_OPTIONS})
    target_link_libraries(${target_name} PRIVATE ${target_name}_recomp)
    target_include_directories(${target_name} PRIVATE "${DANTE_TREE}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE rex::runtime)
    rexglue_configure_target(${target_name} ${ARGN})
endmacro()
