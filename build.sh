#!/bin/bash
#
# Hyperspace ThumbyColor Build Script
# Supports ARM (default) and RISC-V builds
#
# Usage:
#   ./build.sh          # Build for ARM (default)
#   ./build.sh arm      # Build for ARM
#   ./build.sh riscv    # Build for RISC-V
#   ./build.sh clean    # Clean build directory
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

case "${1:-arm}" in
    arm|ARM)
        echo "Building for RP2350 ARM (Cortex-M33)..."
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake ..
        make -j$(nproc)
        echo ""
        echo "Build complete! Output: ${BUILD_DIR}/hyperspace_thumbycolor.uf2"
        ;;
    riscv|RISCV|risc-v|RISC-V)
        echo "Building for RP2350 RISC-V (Hazard3, RV32IMAC)..."
        echo ""
        echo "Requires: CORE-V toolchain (riscv32-corev-elf-gcc)"
        echo ""
        echo "Install: Download from https://buildbot.embecosm.com/job/corev-gcc-ubuntu2204/"
        echo "         Extract and set: export PICO_TOOLCHAIN_PATH=/path/to/corev-openhw-gcc-..."
        echo ""
        echo "Note: gcc-riscv64-unknown-elf does NOT work (RP2350 is 32-bit)"
        echo ""
        rm -rf "$BUILD_DIR"
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake -DRISCV=ON ..
        make -j$(nproc)
        echo ""
        echo "Build complete! Output: ${BUILD_DIR}/hyperspace_thumbycolor.uf2"
        ;;
    clean)
        echo "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        echo "Done."
        ;;
    *)
        echo "Usage: $0 [arm|riscv|clean]"
        echo ""
        echo "  arm    - Build for ARM Cortex-M33 (default)"
        echo "  riscv  - Build for RISC-V"
        echo "  clean  - Remove build directory"
        exit 1
        ;;
esac
