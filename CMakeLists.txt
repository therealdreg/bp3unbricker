# MIT License - bp3unbricker
# ---------------------------------------------------------------------------
# Copyright (c) [2026] by David Reguera Garcia aka Dreg
# https://github.com/therealdreg/bp3unbricker
# https://www.rootkit.es
# X @therealdreg
# dreg@rootkit.es
# ---------------------------------------------------------------------------
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

# Raspberry Pi Pico (RP2040) by default; -DPICO_BOARD=pico2 builds for the
# Pico 2 (RP2350). `python bp3unbricker.py build` builds both into dist/.
set(PICO_BOARD pico CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(bp3unbricker C CXX ASM)

pico_sdk_init()

set(BP3_VERSION 1.3.2)

# The whole firmware is one file (README.md, "The firmware source").
add_executable(bp3unbricker bp3unbricker.c)

target_compile_options(bp3unbricker PRIVATE -Wall -Wextra)
target_compile_definitions(bp3unbricker PRIVATE BP3_VERSION="${BP3_VERSION}" BP3_BOARD="${PICO_BOARD}")

# bp3unbricker-pico.uf2 / bp3unbricker-pico2.uf2: the name says which board it is for.
set_target_properties(bp3unbricker PROPERTIES OUTPUT_NAME bp3unbricker-${PICO_BOARD})

target_link_libraries(bp3unbricker
    pico_stdlib
    pico_rand
    hardware_gpio
    hardware_sync
)

pico_set_program_name(bp3unbricker "bp3unbricker")
pico_set_program_version(bp3unbricker ${BP3_VERSION})
pico_set_program_description(bp3unbricker "Bus Pirate v3 PIC24FJ64GA002 unbricker (ICSP)")

pico_enable_stdio_usb(bp3unbricker 1)
pico_enable_stdio_uart(bp3unbricker 0)

pico_add_extra_outputs(bp3unbricker)
