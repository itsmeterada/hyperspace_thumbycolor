cmake_minimum_required(VERSION 3.13)

# ThumbyColor uses RP2350
set(PICO_PLATFORM rp2350)
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
    ./libfixmath/fix16.c
    ./libfixmath/fix16_sqrt.c
    ./libfixmath/fix16_trig.c
    ./libfixmath/fix16_exp.c
)
target_include_directories(libfixmath PUBLIC ./libfixmath)
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
    libfixmath
)

# Enable USB output for debugging
pico_enable_stdio_usb(hyperspace_thumbycolor 1)
pico_enable_stdio_uart(hyperspace_thumbycolor 0)

# Create UF2 file
pico_add_extra_outputs(hyperspace_thumbycolor)

# Optimize for size
target_compile_options(hyperspace_thumbycolor PRIVATE
    -O3
    -ffast-math
    -ffunction-sections
    -fdata-sections
)

target_link_options(hyperspace_thumbycolor PRIVATE
    -Wl,--gc-sections
)
