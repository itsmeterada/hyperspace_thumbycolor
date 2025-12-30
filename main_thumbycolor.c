/*
 * Hyperspace - ThumbyColor Port
 * Original game by J-Fry for PICO-8
 * ThumbyColor port by itsmeterada
 *
 * Uses libfixmath for fixed-point arithmetic
 * 128x128 RGB565 display (matches PICO-8 resolution)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"
#include "thumbycolor_hw.h"
#include "libfixmath/fixmath.h"

// Flash storage for persistent data
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define FLASH_MAGIC 0x48595045  // "HYPE"

// Screen dimensions (ThumbyColor: 128x128, same as PICO-8!)
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 128

// RGB333 color format (discovered through testing)
// This display actually uses 3 bits per channel (512 colors total)
// - Red: bit 0 (MSB), bits 15-14 (LSB) = 3 bits
// - Green: bits 11-9 = 3 bits
// - Blue: bits 5-3 = 3 bits
// - Other bits (13,12,8,7,6,2,1) are unused

#define RGB565(r, g, b) ( \
    (((r) >> 7) & 0x01)         |  /* R8 bit 7 → output bit 0 (R MSB) */ \
    ((((r) >> 5) & 0x03) << 14) |  /* R8 bits 6-5 → output bits 15-14 (R LSB) */ \
    ((((g) >> 5) & 0x07) << 9)  |  /* G8 bits 7-5 → output bits 11-9 */ \
    ((((b) >> 5) & 0x07) << 3)     /* B8 bits 7-5 → output bits 5-3 */ \
)

// Old incorrect format (for reference)
#define RGB565_OLD(r, g, b) ( \
    (((r) & 0xC0) << 8) |  /* R bits 7-6 → output bits 15-14 */ \
    (((g) & 0xFC) << 6) |  /* G bits 7-2 → output bits 13-8 */ \
    (((b) & 0xF8) >> 0) |  /* B bits 7-3 → output bits 7-3 */ \
    (((r) & 0x38) >> 3)    /* R bits 5-3 → output bits 2-0 */ \
)

// Format B: Standard RGB565 (TinyCircuits style)
#define RGB565_STD(r, g, b) ( \
    (((r) & 0xF8) << 8) |  /* R bits 7-3 → output bits 15-11 */ \
    (((g) & 0xFC) << 3) |  /* G bits 7-2 → output bits 10-5 */ \
    (((b) >> 3) & 0x1F)    /* B bits 7-3 → output bits 4-0 */ \
)

// Format C: Standard RGB565 byte-swapped
#define RGB565_STD_SWAP(r, g, b) ( \
    ((RGB565_STD(r,g,b) >> 8) & 0xFF) | ((RGB565_STD(r,g,b) << 8) & 0xFF00) \
)

// Format D: Standard RGB565 byte-swapped with corrected channel order
// STD_SWAP shows BRG (R→B, G→R, B→G), so input must be (b, r, g)
#define RGB565_STD_SWAP_FIXED(r, g, b) RGB565_STD_SWAP(b, r, g)

// PICO-8 16-color palette in RGB565
static const uint16_t PICO8_PALETTE[16] = {
    RGB565(0x00, 0x00, 0x00), //  0: black        #000000
    RGB565(0x1D, 0x2B, 0x53), //  1: dark blue    #1D2B53
    RGB565(0x7E, 0x25, 0x53), //  2: dark purple  #7E2553
    RGB565(0x00, 0x87, 0x51), //  3: dark green   #008751
    RGB565(0xAB, 0x52, 0x36), //  4: brown        #AB5236
    RGB565(0x5F, 0x57, 0x4F), //  5: dark gray    #5F574F
    RGB565(0xC2, 0xC3, 0xC7), //  6: light gray   #C2C3C7
    RGB565(0xFF, 0xF1, 0xE8), //  7: white        #FFF1E8
    RGB565(0xFF, 0x00, 0x4D), //  8: red          #FF004D
    RGB565(0xFF, 0xA3, 0x00), //  9: orange       #FFA300
    RGB565(0xFF, 0xEC, 0x27), // 10: yellow       #FFEC27
    RGB565(0x00, 0xE4, 0x36), // 11: green        #00E436
    RGB565(0x29, 0xAD, 0xFF), // 12: blue         #29ADFF
    RGB565(0x83, 0x76, 0x9C), // 13: indigo       #83769C
    RGB565(0xFF, 0x77, 0xA8), // 14: pink         #FF77A8
    RGB565(0xFF, 0xCC, 0xAA), // 15: peach        #FFCCAA
};

// Screen buffer (8-bit palette indices)
static uint8_t screen[SCREEN_HEIGHT][SCREEN_WIDTH];

// RGB565 framebuffer for display
static uint16_t framebuffer[SCREEN_HEIGHT * SCREEN_WIDTH];

// Sprite sheet (128x128 pixels, 4-bit palette)
static uint8_t spritesheet[128][128];

// Map memory (for mesh data)
static uint8_t map_memory[0x1000];

// Palette mapping for pal()
static uint8_t palette_map[16];

// Drawing color
static uint8_t draw_color = 7;

// Clip region
static int clip_x1 = 0, clip_y1 = 0;
static int clip_x2 = SCREEN_WIDTH - 1, clip_y2 = SCREEN_HEIGHT - 1;

// Random seed
static uint32_t rnd_state = 1;

// Button states
static bool btn_state[6] = {false};
static bool btn_prev[6] = {false};
static bool btn_menu_held = false;  // Menu button for palette display
static bool btn_bumper_l_held = false;  // L button for color bar test

// Cart data (persistent storage)
static int32_t cart_data[64] = {0};
static bool cart_data_dirty = false;

// Flash storage structure
typedef struct {
    uint32_t magic;
    int32_t data[64];
} FlashSaveData;

static void load_cart_data(void) {
    const FlashSaveData* flash_data = (const FlashSaveData*)(XIP_BASE + FLASH_TARGET_OFFSET);
    if (flash_data->magic == FLASH_MAGIC) {
        memcpy(cart_data, flash_data->data, sizeof(cart_data));
    }
}

static void save_cart_data(void) {
    if (!cart_data_dirty) return;

    FlashSaveData save_data;
    save_data.magic = FLASH_MAGIC;
    memcpy(save_data.data, cart_data, sizeof(cart_data));

    uint8_t buffer[512] __attribute__((aligned(4)));
    memset(buffer, 0xFF, sizeof(buffer));
    memcpy(buffer, &save_data, sizeof(save_data));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, buffer, sizeof(buffer));
    restore_interrupts(ints);

    cart_data_dirty = false;
}

// Fixed-point constants
#define FIX_HALF F16(0.5)
#define FIX_TWO F16(2.0)
#define FIX_PI fix16_pi
#define FIX_TWO_PI F16(6.28318530718)
#define FIX_SCREEN_CENTER F16(64.0)  // 128/2
#define FIX_PROJ_CONST F16(-80.0)    // Projection constant for 128px

// =============================================================================
// PICO-8 API Implementation
// =============================================================================

static void cls(void) {
    memset(screen, 0, sizeof(screen));
}

static void pset(int x, int y, int c) {
    if (x >= clip_x1 && x <= clip_x2 && y >= clip_y1 && y <= clip_y2 &&
        x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        screen[y][x] = palette_map[c & 15];
    }
}

// Fast pset - uses palette mapping for animation
#define PSET_FAST(x, y, c) (screen[(y)][(x)] = palette_map[(c) & 15])

static uint8_t pget(int x, int y) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        return screen[y][x];
    }
    return 0;
}

static uint8_t sget(int x, int y) {
    if (x >= 0 && x < 128 && y >= 0 && y < 128) {
        return spritesheet[y][x];
    }
    return 0;
}

// Fast texture fetch
#define SGET_FAST(x, y) (spritesheet[(y)][(x)])

static void line(int x0, int y0, int x1, int y1, int c) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (1) {
        pset(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void rectfill(int x0, int y0, int x1, int y1, int c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            pset(x, y, c);
        }
    }
}

static void circfill(int cx, int cy, int r, int c) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x*x + y*y <= r*r) {
                pset(cx + x, cy + y, c);
            }
        }
    }
}

static void spr(int n, int x, int y, int w, int h) {
    int sx = (n & 15) * 8;
    int sy = (n / 16) * 8;
    for (int py = 0; py < h * 8; py++) {
        for (int px = 0; px < w * 8; px++) {
            uint8_t c = sget(sx + px, sy + py);
            if (c != 0) {
                pset(x + px, y + py, palette_map[c]);
            }
        }
    }
}

static void pal_reset(void) {
    for (int i = 0; i < 16; i++) palette_map[i] = i;
}

static void pal(int c0, int c1) {
    palette_map[c0 & 15] = c1 & 15;
}

static void clip_set(int x, int y, int w, int h) {
    clip_x1 = x;
    clip_y1 = y;
    clip_x2 = x + w - 1;
    clip_y2 = y + h - 1;
}

static void clip_reset(void) {
    clip_x1 = 0;
    clip_y1 = 0;
    clip_x2 = SCREEN_WIDTH - 1;
    clip_y2 = SCREEN_HEIGHT - 1;
}

static void color(int c) {
    draw_color = c & 15;
}

// =============================================================================
// Platform-specific audio implementation
// =============================================================================

#define PLATFORM_SFX  // Enable platform-specific sfx() implementation

void platform_sfx(int n, int channel) {
    thumbycolor_sfx(n, channel);
}

// =============================================================================
// Include sprite/map data and game logic (shared with other ports)
// =============================================================================

#include "hyperspace_data.h"
#include "hyperspace_game.h"

// =============================================================================
// Button Update
// =============================================================================

static void update_buttons(void) {
    uint32_t buttons = thumbycolor_get_buttons();

    for (int i = 0; i < 6; i++) {
        btn_prev[i] = btn_state[i];
    }

    btn_state[0] = (buttons & BUTTON_LEFT) != 0;
    btn_state[1] = (buttons & BUTTON_RIGHT) != 0;
    btn_state[2] = (buttons & BUTTON_UP) != 0;
    btn_state[3] = (buttons & BUTTON_DOWN) != 0;
    btn_state[4] = (buttons & BUTTON_A) != 0;      // Fire / OK
    btn_state[5] = (buttons & BUTTON_B) != 0;      // Barrel roll

    // Menu button for palette display
    btn_menu_held = (buttons & BUTTON_MENU) != 0;

    // L bumper for color bar test
    btn_bumper_l_held = (buttons & BUTTON_BUMPER_L) != 0;
}

// =============================================================================
// Palette Display (for PICO-8 color comparison)
// =============================================================================

static void draw_palette_display(void) {
    // 4x4 grid of 16 colors
    // Each cell is 32x32 pixels (128/4 = 32)
    const int cell_size = 32;

    for (int i = 0; i < 16; i++) {
        int col = i % 4;
        int row = i / 4;
        int x0 = col * cell_size;
        int y0 = row * cell_size;

        // Fill rectangle with color i (bypass palette_map to show true colors)
        for (int y = y0; y < y0 + cell_size; y++) {
            for (int x = x0; x < x0 + cell_size; x++) {
                screen[y][x] = i;
            }
        }

        // Draw border (color 0 or 7 for contrast)
        int border_color = (i == 0 || i == 1 || i == 2 || i == 5) ? 7 : 0;
        for (int x = x0; x < x0 + cell_size; x++) {
            screen[y0][x] = border_color;
            screen[y0 + cell_size - 1][x] = border_color;
        }
        for (int y = y0; y < y0 + cell_size; y++) {
            screen[y][x0] = border_color;
            screen[y][x0 + cell_size - 1] = border_color;
        }
    }
}

// =============================================================================
// Color Bar Test (direct RGB565 output)
// =============================================================================

static void draw_color_bars_test(void) {
    // Test RGB333 macro with 100% and 50% colors
    // Top: 100%, Bottom: 50%
    // Columns: Red, Green, Blue, White
    const int half_height = SCREEN_HEIGHT / 2;

    // 100% colors
    const uint16_t full[4] = {
        RGB565(0xFF, 0x00, 0x00),  // Red
        RGB565(0x00, 0xFF, 0x00),  // Green
        RGB565(0x00, 0x00, 0xFF),  // Blue
        RGB565(0xFF, 0xFF, 0xFF),  // White
    };

    // 50% colors
    const uint16_t half[4] = {
        RGB565(0x80, 0x00, 0x00),  // 50% Red
        RGB565(0x00, 0x80, 0x00),  // 50% Green
        RGB565(0x00, 0x00, 0x80),  // 50% Blue
        RGB565(0x80, 0x80, 0x80),  // 50% Gray
    };

    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            int col = (x * 4) / SCREEN_WIDTH;
            if (col > 3) col = 3;

            if (y < half_height) {
                framebuffer[y * SCREEN_WIDTH + x] = full[col];
            } else if (y == half_height) {
                framebuffer[y * SCREEN_WIDTH + x] = 0x0E39;  // Gray divider
            } else {
                framebuffer[y * SCREEN_WIDTH + x] = half[col];
            }
        }
    }
}

// =============================================================================
// Screen Buffer to Framebuffer Conversion
// =============================================================================

static void render_to_framebuffer(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            framebuffer[y * SCREEN_WIDTH + x] = PICO8_PALETTE[screen[y][x] & 15];
        }
    }
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(void) {
    // Initialize stdio for USB debugging
    stdio_init_all();

    // Initialize ThumbyColor hardware
    thumbycolor_init();

    // Load sprite and map data (from hyperspace_data.h)
    load_embedded_data();

    // Initialize random seed
    rnd_state = thumbycolor_time_ms();

    // Initialize game
    game_init();

    // Turn off LED
    thumbycolor_set_led(0, 0, 0);

    // Main loop
    while (1) {
        // Update input
        update_buttons();

        // Update and draw game (or show test patterns)
        if (btn_bumper_l_held) {
            // Show color bar test when L is held (direct framebuffer)
            draw_color_bars_test();
            thumbycolor_update(framebuffer);
        } else if (btn_menu_held) {
            // Show palette display when Menu is held
            draw_palette_display();
            render_to_framebuffer();
            thumbycolor_update(framebuffer);
        } else {
            game_update();
            game_draw();
            // Update audio system (advance note playback)
            thumbycolor_audio_update();
            // Convert to RGB565 and send to display
            render_to_framebuffer();
            thumbycolor_update(framebuffer);
        }

        // Wait for vsync (~30 FPS for game)
        thumbycolor_wait_vsync();
    }

    return 0;
}
