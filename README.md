# Hyperspace for Thumby Color

A port of the PICO-8 game "Hyperspace" by J-Fry to the TinyCircuits Thumby Color handheld console.

## Screenshot

![Screenshot](screenshot.jpg)

## Features

- Full 3D software rasterization with texture mapping
- Fixed-point arithmetic optimized for RP2350
- Native PICO-8 resolution (128x128) - no scaling needed!
- 4 enemy types: asteroids, small ships, medium ships, and boss
- Barrel roll maneuver for dodging
- Auto-fire and manual fire modes
- Lens flare effects with temporal dithering
- Persistent high score via flash storage

## Hardware

| Component | Specification |
|-----------|---------------|
| MCU | RP2350 (Dual ARM Cortex-M33 @ 150MHz) |
| Display | GC9107 128x128 0.85" IPS LCD |
| Color | RGB565 (65,536 colors) |
| Interface | SPI @ 80MHz with DMA |

## Building

### Requirements

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) (v2.0.0 or later for RP2350)
- CMake 3.13+
- ARM GCC toolchain

### Environment Setup

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

### Build Steps

```bash
cd thumbycolor
mkdir build
cd build
cmake ..
make -j$(nproc)
```

The output `hyperspace_thumbycolor.uf2` can be copied to the Thumby Color in bootloader mode (hold BOOTSEL while connecting USB).

## Controls

| Button | Action |
|--------|--------|
| D-Pad | Move ship |
| A | Fire laser / Confirm |
| B | Barrel roll |
| Menu | Start game |

## GPIO Pin Mapping

Based on TinyCircuits Thumby Color hardware:

### Buttons (active-low with internal pull-ups)
| Button | GPIO |
|--------|------|
| D-Pad Up | 1 |
| D-Pad Down | 3 |
| D-Pad Left | 0 |
| D-Pad Right | 2 |
| A | 21 |
| B | 25 |
| Left Bumper | 6 |
| Right Bumper | 22 |
| Menu | 26 |

### Display (SPI0)
| Signal | GPIO |
|--------|------|
| MOSI (SDA) | 19 |
| SCK | 18 |
| CS | 17 |
| DC | 16 |
| RST | 4 |
| Backlight | 7 |

### Other Peripherals
| Function | GPIO |
|----------|------|
| Audio PWM | 23 |
| Audio Enable | 20 |
| LED Red | 11 |
| LED Green | 10 |
| LED Blue | 12 |
| Rumble | 5 |
| Battery ADC | 29 |
| Charge Status | 24 |

## Technical Details

### Display Driver (GC9107)

- SPI0 at 80MHz for pixel data (16-bit transfers)
- DMA for efficient framebuffer transfer
- PWM backlight brightness control
- Display inversion enabled for correct colors
- Custom gamma curves for improved brightness

### Color Format

- Internal: 8-bit palette indices (PICO-8 16-color palette)
- Display: RGB565 (16-bit, R and B channels swapped for GC9107)
- Palette animation supported via `pal()` function

### Rendering Pipeline

1. **Mesh Loading**: Meshes decoded from embedded map memory
2. **Matrix Transformations**: 3x4 matrices for rotation and translation
3. **Projection**: Perspective projection with 128px screen center
4. **Triangle Rasterization**: Scanline-based with barycentric interpolation
5. **Texture Mapping**: UV coordinates with perspective correction
6. **Lighting**: Per-triangle lighting with dithering

### Memory Usage

| Section | Description |
|---------|-------------|
| Spritesheet | 16KB (128x128 4-bit pixels) |
| Map Memory | 4KB (mesh definitions) |
| Screen Buffer | 16KB (128x128 8-bit palette) |
| Framebuffer | 32KB (128x128 RGB565) |

### Advantages over PicoSystem Version

| Feature | Thumby Color | PicoSystem |
|---------|--------------|------------|
| Resolution | 128x128 (native PICO-8) | 120x120 |
| Color Depth | RGB565 (65K) | RGBA4444 (4K) |
| SPI Speed | 80MHz | ~62MHz |
| MCU | RP2350 | RP2040 |

## Project Structure

```
thumbycolor/
├── main_thumbycolor.c    # Main game code
├── thumbycolor_hw.c      # Hardware abstraction layer
├── thumbycolor_hw.h      # HAL header
├── CMakeLists.txt        # Build configuration
└── build/                # Build output directory
    └── hyperspace_thumbycolor.uf2
```

## Related Projects

- [Hyperspace for PicoSystem](https://github.com/itsmeterada/picosystem_hyperspace) - Port for Pimoroni PicoSystem
- [Hyperspace for GBA](https://github.com/itsmeterada/hyperspace_gba) - Port for Game Boy Advance
- [Hyperspace SDL2](https://github.com/itsmeterada/hyperspace) - SDL2 port for desktop platforms

## Credits

- **Original Game**: [Hyperspace](https://www.lexaloffle.com/bbs/?tid=41663) by J-Fry (PICO-8)
- **Thumby Color Port**: itsmeterada
- **libfixmath**: [PetteriAimworthy/libfixmath](https://github.com/PetteriAimworthy/libfixmath)

## License

This port is provided for educational and personal use. Please respect the original author's rights.
