# Hyperspace - ThumbyColor Port

A port of Hyperspace to the ThumbyColor handheld console.

## Screenshot

![Screenshot](screenshot.jpg)

## Hardware

- **MCU**: RP2350 (or RP2040 compatible)
- **Display**: GC9107 128x128 0.85" IPS LCD (RGB565, SPI)
- **Resolution**: 128x128 (matches PICO-8 exactly!)

## Original Game

- **Original**: [Hyperspace by J-Fry](https://www.lexaloffle.com/bbs/?tid=51336) for PICO-8
- **ThumbyColor port**: itsmeterada

## GPIO Pin Mapping

Based on TinyCircuits Tiny Game Engine:

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

### Other
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

## Building

### Requirements

- [Pico SDK](https://github.com/raspberrypi/pico-sdk)
- CMake 3.13+
- ARM GCC toolchain

### Build Steps

```bash
cd thumbycolor
mkdir build
cd build
cmake ..
make -j$(nproc)
```

The output `hyperspace_thumbycolor.uf2` can be copied to the ThumbyColor in bootloader mode.

## Controls

| Button | Action |
|--------|--------|
| D-Pad | Move ship |
| A | Fire laser |
| B | Barrel roll |
| Menu | Pause (TBD) |

## Technical Details

### Display Driver

The GC9107 driver uses:
- SPI0 at 80MHz for pixel data (16-bit transfers)
- DMA for efficient framebuffer transfer
- PWM for backlight brightness control

### Color Format

- Internal: 8-bit palette indices (PICO-8 compatible)
- Display: RGB565 (16-bit, same as GBA)
- Palette animation supported via `pal()` function

### Advantages over PicoSystem

- **Native PICO-8 resolution**: 128x128 vs 120x120
- **Better color**: RGB565 (65K colors) vs RGBA4444
- **Faster SPI**: 80MHz vs ~62MHz

## Status

- [x] Hardware abstraction layer
- [x] GC9107 LCD driver
- [x] Button input
- [x] PWM backlight
- [ ] Full game port
- [ ] Audio support
- [ ] Rumble feedback

## License

Based on the original PICO-8 game by J-Fry.
