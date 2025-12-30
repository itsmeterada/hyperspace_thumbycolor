cmake_minimum_required(VERSION 3.13)

# Option to build for RISC-V instead of ARM
# Usage:
#   ARM build (default):  cmake ..
#   RISC-V build:         cmake -DRISCV=ON ..
#
# For RISC-V, you need a 32-bit RISC-V toolchain.
# Pico SDK looks for: riscv32-unknown-elf-gcc or riscv32-corev-elf-gcc
#
# Recommended: CORE-V Toolchain (no symlinks needed!)
#   Download from: https://buildbot.embecosm.com/job/corev-gcc-ubuntu2204/
#   Extract and set environment:
#     export PICO_TOOLCHAIN_PATH=/path/to/corev-openhw-gcc-ubuntu2204-YYYYMMDD
#   Reference: https://www.cnx-software.com/2024/08/31/using-risc-v-cores-on-the-raspberry-pi-pico-2-board-and-rp2350-mcu-from-blinking-an-led-to-building-linux/
#
# Note: gcc-riscv64-unknown-elf does NOT work - RP2350 uses 32-bit RISC-V (Hazard3)
option(RISCV "Build for RISC-V architecture" OFF)

# ThumbyColor uses RP2350
if(RISCV)
    message(STATUS "========================================")
    message(STATUS "Building for RP2350 RISC-V")
    message(STATUS "========================================")
    set(PICO_PLATFORM rp2350-riscv)
else()
    message(STATUS "========================================")
    message(STATUS "Building for RP2350 ARM (Cortex-M33)")
    message(STATUS "========================================")
    set(PICO_PLATFORM rp2350-arm-s)
endif()

set(PICO_BOARD pico2)

# Include Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(hyperspace_thumbycolor C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# Add libfixmath
add_library(libfixmath STATIC
    ../libfixmath/fix16.c
    ../libfixmath/fix16_sqrt.c
    ../libfixmath/fix16_trig.c
    ../libfixmath/fix16_exp.c
)
target_include_directories(libfixmath PUBLIC ../libfixmath)
target_compile_definitions(libfixmath PUBLIC FIXMATH_NO_OVERFLOW)

# Main executable
add_executable(hyperspace_thumbycolor
    main_thumbycolor.c
    thumbycolor_hw.c
)

target_include_directories(hyperspace_thumbycolor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(hyperspace_thumbycolor
    pico_stdlib
    hardware_spi
    hardware_dma
    hardware_pwm
    hardware_gpio
    hardware_adc
    hardware_flash
    hardware_sync
    hardware_timer
    pico_time
    libfixmath
)

# Enable USB output for debugging
pico_enable_stdio_usb(hyperspace_thumbycolor 1)
pico_enable_stdio_uart(hyperspace_thumbycolor 0)

# Create UF2 file
pico_add_extra_outputs(hyperspace_thumbycolor)

# Optimize for speed
target_compile_options(hyperspace_thumbycolor PRIVATE
    -O3
    -ffast-math
    -ffunction-sections
    -fdata-sections
)

target_link_options(hyperspace_thumbycolor PRIVATE
    -Wl,--gc-sections
)
