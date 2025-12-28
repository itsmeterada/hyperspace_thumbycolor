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

// RGB565 color macro (R and B swapped for this display)
#define RGB565(r, g, b) (((b & 0xF8) << 8) | ((g & 0xFC) << 3) | (r >> 3))

// PICO-8 16-color palette in RGB565
static const uint16_t PICO8_PALETTE[16] = {
    RGB565(0x00, 0x00, 0x00), //  0: black
    RGB565(0x1D, 0x2B, 0x53), //  1: dark blue
    RGB565(0x7E, 0x25, 0x53), //  2: dark purple
    RGB565(0x00, 0x87, 0x51), //  3: dark green
    RGB565(0xAB, 0x52, 0x36), //  4: brown
    RGB565(0x5F, 0x57, 0x4F), //  5: dark gray
    RGB565(0xC2, 0xC3, 0xC7), //  6: light gray
    RGB565(0xFF, 0xF1, 0xE8), //  7: white
    RGB565(0xFF, 0x00, 0x4D), //  8: red
    RGB565(0xFF, 0xA3, 0x00), //  9: orange
    RGB565(0xFF, 0xEC, 0x27), // 10: yellow
    RGB565(0x00, 0xE4, 0x36), // 11: green
    RGB565(0x29, 0xAD, 0xFF), // 12: blue
    RGB565(0x83, 0x76, 0x9C), // 13: indigo
    RGB565(0xFF, 0x77, 0xA8), // 14: pink
    RGB565(0xFF, 0xCC, 0xAA), // 15: peach
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

        // Update and draw game
        game_update();
        game_draw();

        // Convert to RGB565 and send to display
        render_to_framebuffer();
        thumbycolor_update(framebuffer);

        // Wait for vsync (~30 FPS for game)
        thumbycolor_wait_vsync();
    }

    return 0;
}
