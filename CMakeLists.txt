# state-designer -- a visual state machine designer, live simulator, and
# Ordo C++ code generator. This is the repo's build entry point: it resolves
# the ordo framework, then builds the app out of src/.
#
# Build from a checkout:
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt6>
#   cmake --build build
#   ./build/state-designer                       # the app
#   ./build/state-designer --smoke               # headless self-check, exit 0
#   ./build/state-designer --gui-probe           # shell tour + scenarios
#   ./build/state-designer --codegen-check [dir] # headless validate+generate+write
#
# The smoke resolves machine paths repo-root-relative, so run the exe from the
# repo root, not from inside build/.
cmake_minimum_required(VERSION 3.24)

project(state-designer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Build options --------------------------------------------------------
option(ENABLE_LOGGING "Enable runtime file logging and routing trace diagnostics" ON)
option(ENABLE_DEV_DEMO "Load initial demo machines (Login Flow, Traffic Light) at boot" OFF)

# --- ordo framework resolution -------------------------------------------
# ordo is an external, required dependency: the app links ordo::qt, and the
# code generator emits `#include <ordo/core/agent.h>` into every generated
# machine. Resolution order (there is no FetchContent fallback, so a build
# never silently uses a different ordo than the one checked out):
#   1. -DORDO_DIR=<path>       -- explicit; takes precedence.
#   2. third_party/ordo/       -- an in-tree copy.
#   3. ../ordo                 -- a sibling checkout.
set(ORDO_DIR "" CACHE PATH "Path to an ordo framework checkout (overrides the in-tree mirror and the sibling fallback)")

if(ORDO_DIR)
    set(_ordo_root "${ORDO_DIR}")
    set(_ordo_origin "ORDO_DIR")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ordo/CMakeLists.txt")
    set(_ordo_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ordo")
    set(_ordo_origin "in-tree mirror")
else()
    set(_ordo_root "${CMAKE_CURRENT_SOURCE_DIR}/../ordo")
    set(_ordo_origin "sibling checkout")
endif()

get_filename_component(_ordo_root "${_ordo_root}" ABSOLUTE)

if(NOT EXISTS "${_ordo_root}/CMakeLists.txt")
    message(FATAL_ERROR
        "ordo framework not found at ${_ordo_root} (tried: ${_ordo_origin}).\n"
        "state-designer links ordo::qt and its generated code includes "
        "<ordo/core/agent.h>, so the framework is required.\n"
        "Point at a checkout with -DORDO_DIR=<path>, or place one at "
        "third_party/ordo, or clone it next to this repo as ../ordo.")
endif()

message(STATUS "state-designer: using ordo from ${_ordo_root} (${_ordo_origin})")

# --- Debug info, always ---------------------------------------------------
# -g only, with optimization unchanged, so the crash handler's frame addresses
# resolve through addr2line. Set before add_subdirectory so ordo's targets
# carry symbols too.
#
# -fno-dwarf2-cfi-asm works around a MinGW assembler limit: with .cfi_*
# directives GCC emits one `.debug_frame$<mangled>` COMDAT per function, and the
# large std::variant in src/model/undo_store.h overflows as.exe's string table
# ("string table overflow" / "too many sections"). The flag merges CFI into a
# single .debug_frame section; .debug_info and .debug_line are unaffected.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-g -fno-dwarf2-cfi-asm)

    # Fast multi-threaded linking via LLVM lld if present.
    # GNU ld.bfd is single-threaded and takes 4+ minutes on ~180MB DWARF debug info.
    # lld links PE/COFF binaries in seconds using all CPU cores.
    find_program(LLD_EXECUTABLE NAMES ld.lld lld HINTS "C:/Program Files/LLVM/bin" "C:/PROGRA~1/LLVM/bin")
    if(LLD_EXECUTABLE)
        get_filename_component(LLD_DIR "${LLD_EXECUTABLE}" DIRECTORY)
        if(CMAKE_HOST_WIN32 AND LLD_DIR MATCHES " ")
            if(EXISTS "C:/PROGRA~1/LLVM/bin/ld.lld.exe")
                set(LLD_DIR "C:/PROGRA~1/LLVM/bin")
            else()
                execute_process(
                    COMMAND cmd.exe /c for %I in ("${LLD_DIR}") do @echo %~sI
                    OUTPUT_VARIABLE _lld_short_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )
                if(_lld_short_dir)
                    file(TO_CMAKE_PATH "${_lld_short_dir}" LLD_DIR)
                endif()
            endif()
        endif()
        add_link_options("-B${LLD_DIR}" "-fuse-ld=lld")
        message(STATUS "Linker: using LLVM lld from ${LLD_DIR}")
    endif()
endif()

# ordo's own test suite is its repo's concern; ORDO_BUILD_TESTS already
# defaults to PROJECT_IS_TOP_LEVEL, which is false here, but state it so a
# stray cache entry cannot drag ordo's gtests into this build.
set(ORDO_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${_ordo_root}" ordo)

add_subdirectory(src)
