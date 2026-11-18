cmake_minimum_required(VERSION 3.15)

# ─── Project ──────────────────────────────────────────────────────────────────
#
# cemu-headless: Headless CLI binary for programmatic TI-84 Plus CE control.
#
# STATUS: Scaffolding only (Pass 69). CEmu core source files are not yet present
# in this tree. The CMake structure is complete and correct; the binary cannot
# be linked until the CEmu core is added in a future pass.
#
# How to add CEmu core:
#   1. Clone the upstream repository into cemu-core/:
#        git clone https://github.com/CE-Programming/CEmu.git cemu-core
#   2. Re-run CMake — the CEMU_CORE_DIR variable will resolve automatically.
#   3. Build the headless binary.
#
# See README.md for full build instructions and platform notes.

project(cemu-headless
    VERSION     0.1.0
    DESCRIPTION "Headless CLI driver for CEmu TI-84 Plus CE emulator (CalcEngine testing)"
    LANGUAGES   C CXX
)

# ─── C++ standard ─────────────────────────────────────────────────────────────
# CEmu core requires C++17. The CLI entry point (main.c) is plain C99.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ─── Build options ────────────────────────────────────────────────────────────
option(CEMU_HEADLESS_USE_STB_IMAGE
    "Use stb_image_write (single-header) for PNG LCD export"
    ON)

option(CEMU_HEADLESS_UNTHROTTLED
    "Build with unthrottled emulation speed as the default (no QTimer throttle)"
    ON)

# ─── Locate CEmu core ─────────────────────────────────────────────────────────
# The upstream CEmu repository should be cloned to one of the paths below.
# Set -DCEMU_CORE_DIR=<path> on the cmake command line to override.
if(NOT DEFINED CEMU_CORE_DIR)
    # Preferred: sibling directory within this checkout
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cemu-core/core")
        set(CEMU_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cemu-core")
    # Alternate: upstream clone adjacent to the CalcEngine repo
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../CEmu/core")
        set(CEMU_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../CEmu")
    else()
        # Core not found — set to empty so the warning fires below
        set(CEMU_CORE_DIR "")
    endif()
endif()

# Emit a clear diagnostic when the core is missing, but still build the
# autotester-wrapper binary so Phase 2 is usable before the native fork lands.
if(NOT CEMU_CORE_DIR OR NOT EXISTS "${CEMU_CORE_DIR}/core")
    set(HAS_CEMU_CORE OFF)
    message(STATUS
        "[cemu-headless] CEmu core directory not found.\n"
        "  Expected: ${CMAKE_CURRENT_SOURCE_DIR}/cemu-core/core\n"
        "  Native core wiring is disabled for this build.\n"
        "  Wrapper mode remains available and will replay commands through autotester.exe."
    )
else()
    set(HAS_CEMU_CORE ON)
    message(STATUS "[cemu-headless] Using CEmu core at: ${CEMU_CORE_DIR}")
    # ZLIB is only needed if the GUI is also compiled. The headless core does not
    # use zlib directly, so make it optional here. If the build fails with linker
    # errors about z_stream / inflate, install zlib and re-run cmake with ZLIB_ROOT set.
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        message(STATUS "[cemu-headless] zlib found: ${ZLIB_VERSION_STRING}")
    else()
        message(STATUS "[cemu-headless] zlib NOT found — omitting zlib link; core does not need it directly")
    endif()
endif()

# ─── Vendor: stb_image_write ──────────────────────────────────────────────────
# stb_image_write is a single-header PNG writer with no external dependencies.
# Place stb_image_write.h in vendor/ (or the build will use a stub PNG exporter).
set(STB_IMAGE_WRITE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb_image_write.h")
if(CEMU_HEADLESS_USE_STB_IMAGE AND EXISTS "${STB_IMAGE_WRITE_HEADER}")
    set(HAS_STB_IMAGE_WRITE ON)
    message(STATUS "[cemu-headless] stb_image_write found: PNG export enabled")
else()
    set(HAS_STB_IMAGE_WRITE OFF)
    if(CEMU_HEADLESS_USE_STB_IMAGE)
        message(STATUS "[cemu-headless] stb_image_write NOT found at ${STB_IMAGE_WRITE_HEADER} "
                       "— PNG export will use stub (prints 'PNG export not available')")
    endif()
endif()

# ─── CEmu core source files ───────────────────────────────────────────────────
# Explicitly list the core source files that the headless binary needs.
# GUI-only files (gui/, lcd.cpp OpenGL widget, etc.) are intentionally excluded.
# This list must be updated if upstream CEmu reorganises its core/ directory.
set(CEMU_CORE_SOURCES_FOUND "")
if(HAS_CEMU_CORE)
    set(CEMU_CORE_SOURCES
        "${CEMU_CORE_DIR}/core/asic.c"
        "${CEMU_CORE_DIR}/core/backlight.c"
        "${CEMU_CORE_DIR}/core/bootver.c"
        "${CEMU_CORE_DIR}/core/bus.c"
        "${CEMU_CORE_DIR}/core/cert.c"
        "${CEMU_CORE_DIR}/core/control.c"
        "${CEMU_CORE_DIR}/core/cpu.c"
        "${CEMU_CORE_DIR}/core/debug/debug.c"
        "${CEMU_CORE_DIR}/core/emu.c"
        "${CEMU_CORE_DIR}/core/extras.c"
        "${CEMU_CORE_DIR}/core/flash.c"
        "${CEMU_CORE_DIR}/core/interrupt.c"
        "${CEMU_CORE_DIR}/core/keypad.c"
        "${CEMU_CORE_DIR}/core/lcd.c"
        "${CEMU_CORE_DIR}/core/link.c"
        "${CEMU_CORE_DIR}/core/mem.c"
        "${CEMU_CORE_DIR}/core/misc.c"
        "${CEMU_CORE_DIR}/core/panel.c"
        "${CEMU_CORE_DIR}/core/port.c"
        "${CEMU_CORE_DIR}/core/realclock.c"
        "${CEMU_CORE_DIR}/core/registers.c"
        "${CEMU_CORE_DIR}/core/schedule.c"
        "${CEMU_CORE_DIR}/core/sha256.c"
        "${CEMU_CORE_DIR}/core/spi.c"
        "${CEMU_CORE_DIR}/core/timers.c"
        "${CEMU_CORE_DIR}/core/uart.c"
        "${CEMU_CORE_DIR}/core/vat.c"
        "${CEMU_CORE_DIR}/core/usb/disconnected.c"
        "${CEMU_CORE_DIR}/core/usb/dusb.c"
        "${CEMU_CORE_DIR}/core/usb/msd.c"
        "${CEMU_CORE_DIR}/core/usb/physical.c"
        "${CEMU_CORE_DIR}/core/usb/usb.c"
    )

    # Platform-specific OS abstraction
    if(WIN32)
        list(APPEND CEMU_CORE_SOURCES "${CEMU_CORE_DIR}/core/os/os-win32.c")
    else()
        list(APPEND CEMU_CORE_SOURCES "${CEMU_CORE_DIR}/core/os/os-linux.c")
    endif()

    # Filter to only files that actually exist in the checkout.
    # This allows CMake to configure partially (e.g. when core is freshly cloned but
    # some files have been reorganised upstream) without a hard configure error.
    foreach(_src IN LISTS CEMU_CORE_SOURCES)
        if(EXISTS "${_src}")
            list(APPEND CEMU_CORE_SOURCES_FOUND "${_src}")
        else()
            message(WARNING "[cemu-headless] Core source not found (will be missing from link): ${_src}")
        endif()
    endforeach()
endif()

# ─── CLI entry point sources ──────────────────────────────────────────────────
set(CLI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
)

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(cemu-headless
    ${CLI_SOURCES}
    ${CEMU_CORE_SOURCES_FOUND}
)

# ─── Include paths ────────────────────────────────────────────────────────────
target_include_directories(cemu-headless PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
)

if(HAS_CEMU_CORE)
    target_include_directories(cemu-headless PRIVATE
        "${CEMU_CORE_DIR}/core"
    )
endif()

# ─── Compile definitions ──────────────────────────────────────────────────────
target_compile_definitions(cemu-headless PRIVATE
    CEMU_HEADLESS=1
)

if(HAS_CEMU_CORE)
    target_compile_definitions(cemu-headless PRIVATE
        CEMU_HEADLESS_HAS_CEMU_CORE=1
    )
else()
    target_compile_definitions(cemu-headless PRIVATE
        CEMU_HEADLESS_HAS_CEMU_CORE=0
    )
endif()

if(HAS_STB_IMAGE_WRITE)
    target_compile_definitions(cemu-headless PRIVATE
        CEMU_HEADLESS_PNG_EXPORT=1
    )
endif()

if(CEMU_HEADLESS_UNTHROTTLED)
    target_compile_definitions(cemu-headless PRIVATE
        CEMU_HEADLESS_UNTHROTTLED=1
    )
endif()

# ─── Link libraries ───────────────────────────────────────────────────────────
if(HAS_CEMU_CORE AND ZLIB_FOUND)
    target_link_libraries(cemu-headless PRIVATE
        ZLIB::ZLIB
    )
endif()

# On Linux/macOS link pthreads only for native-core builds.
if(UNIX AND HAS_CEMU_CORE)
    find_package(Threads REQUIRED)
    target_link_libraries(cemu-headless PRIVATE Threads::Threads)
endif()

# ─── Platform compile flags ───────────────────────────────────────────────────
if(MSVC)
    target_compile_options(cemu-headless PRIVATE
        /W3
        /D_CRT_SECURE_NO_WARNINGS
        # C11 mode: required for compound literals (e.g. panel.c) and C99/C11 features.
        # MSVC 19.28+ (VS 2019 16.8+) supports /std:c11.
        $<$<COMPILE_LANGUAGE:C>:/std:c11>
    )
else()
    target_compile_options(cemu-headless PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endif()

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS cemu-headless
    RUNTIME DESTINATION bin
)

# ─── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "[cemu-headless] Configuration summary:")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  CEmu core:     ${CEMU_CORE_DIR}")
message(STATUS "  Native core:   ${HAS_CEMU_CORE}")
if(HAS_CEMU_CORE AND ZLIB_FOUND)
    message(STATUS "  zlib:          ${ZLIB_VERSION_STRING}")
elseif(HAS_CEMU_CORE)
    message(STATUS "  zlib:          not found (not required for headless core)")
endif()
message(STATUS "  PNG export:    ${HAS_STB_IMAGE_WRITE}")
message(STATUS "  Unthrottled:   ${CEMU_HEADLESS_UNTHROTTLED}")
message(STATUS "  C++ standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")
