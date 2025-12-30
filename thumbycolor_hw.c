/*
 * ThumbyColor Hardware Abstraction Layer Implementation
 * Based on TinyCircuits Tiny Game Engine
 *
 * GC9107 LCD Driver + Button Input + PWM Backlight
 */

#include "thumbycolor_hw.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include <string.h>
#include <stdlib.h>

// SPI instance
#define SPI_PORT spi0
#define SPI_BAUDRATE_CMD  (10 * 1000 * 1000)   // 10 MHz for commands
#define SPI_BAUDRATE_DATA (80 * 1000 * 1000)   // 80 MHz for pixel data

// DMA channel for display
static int dma_channel = -1;
static dma_channel_config dma_config;

// PWM slice for backlight
static uint backlight_slice;

// =============================================================================
// GC9107 LCD Driver
// =============================================================================

// GC9107 Commands
#define GC9107_SLPOUT   0x11    // Sleep Out
#define GC9107_DISPOFF  0x28    // Display OFF
#define GC9107_DISPON   0x29    // Display ON
#define GC9107_CASET    0x2A    // Column Address Set
#define GC9107_RASET    0x2B    // Row Address Set
#define GC9107_RAMWR    0x2C    // Memory Write
#define GC9107_MADCTL   0x36    // Memory Data Access Control
#define GC9107_COLMOD   0x3A    // Interface Pixel Format

static void gc9107_set_dc(bool data) {
    gpio_put(GPIO_DC, data);
}

static void gc9107_set_cs(bool selected) {
    gpio_put(GPIO_SPI_CS, !selected);  // Active low
}

static void gc9107_write_cmd(uint8_t cmd) {
    gc9107_set_cs(true);
    gc9107_set_dc(false);  // Command mode
    spi_write_blocking(SPI_PORT, &cmd, 1);
    gc9107_set_dc(true);   // Data mode
}

static void gc9107_write_data(const uint8_t *data, size_t len) {
    spi_write_blocking(SPI_PORT, data, len);
}

static void gc9107_write_data8(uint8_t data) {
    spi_write_blocking(SPI_PORT, &data, 1);
}

static void gc9107_cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len) {
    gc9107_write_cmd(cmd);
    if (len > 0) {
        gc9107_write_data(data, len);
    }
    gc9107_set_cs(false);
}

// Display offset (GC9107 has 128x160 RAM, display is 128x128)
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

static void gc9107_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    // Apply offset
    x0 += DISPLAY_OFFSET_X;
    x1 += DISPLAY_OFFSET_X;
    y0 += DISPLAY_OFFSET_Y;
    y1 += DISPLAY_OFFSET_Y;

    // Column address set
    gc9107_write_cmd(GC9107_CASET);
    uint8_t col_data[] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    gc9107_write_data(col_data, 4);
    gc9107_set_cs(false);

    // Row address set
    gc9107_write_cmd(GC9107_RASET);
    uint8_t row_data[] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };
    gc9107_write_data(row_data, 4);
    gc9107_set_cs(false);
}

static void gc9107_init_sequence(void) {
    // Hardware reset (from TinyCircuits timing)
    gpio_put(GPIO_RST, false);
    sleep_ms(50);
    gpio_put(GPIO_RST, true);
    sleep_ms(120);

    // Use 8-bit SPI for initialization (Mode 0 for commands)
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(SPI_PORT, SPI_BAUDRATE_CMD);

    // Inter-register enable commands
    gc9107_cmd_with_data(0xFE, NULL, 0);
    gc9107_cmd_with_data(0xEF, NULL, 0);

    // Power control registers (TinyCircuits values)
    gc9107_cmd_with_data(0xB0, (uint8_t[]){0xC0}, 1);
    gc9107_cmd_with_data(0xB1, (uint8_t[]){0x80}, 1);
    gc9107_cmd_with_data(0xB2, (uint8_t[]){0x2F}, 1);  // Was 0x3d
    gc9107_cmd_with_data(0xB3, (uint8_t[]){0x03}, 1);  // Was 0x1f
    gc9107_cmd_with_data(0xB7, (uint8_t[]){0x01}, 1);  // Was 0x35
    gc9107_cmd_with_data(0xB6, (uint8_t[]){0x19}, 1);  // NEW

    // RGB565 complement setting
    gc9107_cmd_with_data(0xAC, (uint8_t[]){0xC8}, 1);
    gc9107_cmd_with_data(0xAB, (uint8_t[]){0x0F}, 1);

    // Interface Pixel Format: 16-bit RGB565
    gc9107_cmd_with_data(GC9107_COLMOD, (uint8_t[]){0x05}, 1);

    // Display control registers (TinyCircuits values)
    gc9107_cmd_with_data(0xB4, (uint8_t[]){0x04}, 1);  // NEW
    gc9107_cmd_with_data(0xA8, (uint8_t[]){0x07}, 1);  // NEW - Frame rate
    gc9107_cmd_with_data(0xB8, (uint8_t[]){0x08}, 1);  // NEW

    // Voltage regulation (TinyCircuits values) - CRITICAL FOR COLOR
    gc9107_cmd_with_data(0xE7, (uint8_t[]){0x5A}, 1);  // VREG_CTL
    gc9107_cmd_with_data(0xE8, (uint8_t[]){0x23}, 1);  // VGH_SET
    gc9107_cmd_with_data(0xE9, (uint8_t[]){0x47}, 1);  // VGL_SET
    gc9107_cmd_with_data(0xEA, (uint8_t[]){0x99}, 1);  // VGH_VGL_CLK

    // Gamma/contrast control
    gc9107_cmd_with_data(0xC6, (uint8_t[]){0x30}, 1);  // Was 0x0f
    gc9107_cmd_with_data(0xC7, (uint8_t[]){0x1F}, 1);  // NEW

    // Memory Data Access Control (0x00 = no rotation)
    gc9107_cmd_with_data(GC9107_MADCTL, (uint8_t[]){0x00}, 1);

    // Gamma curves (TinyCircuits values - 14 bytes each)
    gc9107_cmd_with_data(0xF0, (uint8_t[]){
        0x05, 0x1D, 0x51, 0x2F, 0x85, 0x2A, 0x11,
        0x62, 0x00, 0x07, 0x07, 0x0F, 0x08, 0x1F
    }, 14);

    gc9107_cmd_with_data(0xF1, (uint8_t[]){
        0x2E, 0x41, 0x62, 0x56, 0xA5, 0x3A, 0x3F,
        0x60, 0x0F, 0x07, 0x0A, 0x18, 0x18, 0x1D
    }, 14);

    // Sleep out (AFTER register setup, per TinyCircuits)
    gc9107_cmd_with_data(GC9107_SLPOUT, NULL, 0);
    sleep_ms(120);

    // Display ON
    gc9107_cmd_with_data(GC9107_DISPON, NULL, 0);
    sleep_ms(10);

    // Set display window to full screen
    gc9107_set_window(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    // Clear ENTIRE display RAM (128x160) with black pixels
    // GC9107 has 128x160 RAM even though display is 128x128
    gc9107_set_window(0, 0, 127, 159);  // Full RAM area
    gc9107_write_cmd(GC9107_RAMWR);
    // 128 * 160 * 2 = 40960 bytes
    for (int i = 0; i < 128 * 160 * 2; i++) {
        uint8_t zero = 0x00;
        spi_write_blocking(SPI_PORT, &zero, 1);
    }
    gc9107_set_cs(false);

    // Switch to 16-bit SPI Mode 3 for pixel data (TinyCircuits uses CPOL=1, CPHA=1)
    spi_set_format(SPI_PORT, 16, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    spi_set_baudrate(SPI_PORT, SPI_BAUDRATE_DATA);
}

// =============================================================================
// Button Input
// =============================================================================

static void buttons_init(void) {
    // Initialize all button GPIOs
    const uint button_pins[] = {
        GPIO_BUTTON_DPAD_UP,
        GPIO_BUTTON_DPAD_LEFT,
        GPIO_BUTTON_DPAD_DOWN,
        GPIO_BUTTON_DPAD_RIGHT,
        GPIO_BUTTON_A,
        GPIO_BUTTON_B,
        GPIO_BUTTON_BUMPER_LEFT,
        GPIO_BUTTON_BUMPER_RIGHT,
        GPIO_BUTTON_MENU
    };

    for (size_t i = 0; i < sizeof(button_pins) / sizeof(button_pins[0]); i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
    }
}

uint32_t thumbycolor_get_buttons(void) {
    uint32_t buttons = 0;

    // Buttons are active-low (grounded when pressed)
    if (!gpio_get(GPIO_BUTTON_DPAD_UP))     buttons |= BUTTON_UP;
    if (!gpio_get(GPIO_BUTTON_DPAD_DOWN))   buttons |= BUTTON_DOWN;
    if (!gpio_get(GPIO_BUTTON_DPAD_LEFT))   buttons |= BUTTON_LEFT;
    if (!gpio_get(GPIO_BUTTON_DPAD_RIGHT))  buttons |= BUTTON_RIGHT;
    if (!gpio_get(GPIO_BUTTON_A))           buttons |= BUTTON_A;
    if (!gpio_get(GPIO_BUTTON_B))           buttons |= BUTTON_B;
    if (!gpio_get(GPIO_BUTTON_BUMPER_LEFT)) buttons |= BUTTON_BUMPER_L;
    if (!gpio_get(GPIO_BUTTON_BUMPER_RIGHT))buttons |= BUTTON_BUMPER_R;
    if (!gpio_get(GPIO_BUTTON_MENU))        buttons |= BUTTON_MENU;

    return buttons;
}

bool thumbycolor_button_pressed(uint32_t button) {
    return (thumbycolor_get_buttons() & button) != 0;
}

// =============================================================================
// PWM Backlight
// =============================================================================

static void backlight_init(void) {
    gpio_set_function(GPIO_BACKLIGHT, GPIO_FUNC_PWM);
    backlight_slice = pwm_gpio_to_slice_num(GPIO_BACKLIGHT);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 255);
    pwm_init(backlight_slice, &config, true);

    // Start at full brightness
    pwm_set_gpio_level(GPIO_BACKLIGHT, 255);
}

void thumbycolor_set_backlight(float brightness) {
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    pwm_set_gpio_level(GPIO_BACKLIGHT, (uint16_t)(brightness * 255.0f));
}

// =============================================================================
// DMA Display Update
// =============================================================================

static void dma_init_display(void) {
    dma_channel = dma_claim_unused_channel(true);
    dma_config = dma_channel_get_default_config(dma_channel);

    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_16);
    channel_config_set_dreq(&dma_config, spi_get_dreq(SPI_PORT, true));
    channel_config_set_read_increment(&dma_config, true);
    channel_config_set_write_increment(&dma_config, false);
}

void thumbycolor_update(uint16_t *framebuffer) {
    // Wait for previous DMA transfer to complete
    if (dma_channel >= 0) {
        dma_channel_wait_for_finish_blocking(dma_channel);
    }

    // Wait for SPI to finish any pending transfers
    while (spi_is_busy(SPI_PORT)) {
        tight_loop_contents();
    }

    // Switch to 8-bit for command
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Set window to full screen
    gc9107_set_window(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    // Start memory write
    gc9107_write_cmd(GC9107_RAMWR);

    // Wait for SPI to finish command
    while (spi_is_busy(SPI_PORT)) {
        tight_loop_contents();
    }

    // Switch to 16-bit SPI Mode 3 for pixel data (TinyCircuits uses CPOL=1, CPHA=1)
    spi_set_format(SPI_PORT, 16, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

    // DMA transfer
    dma_channel_configure(
        dma_channel,
        &dma_config,
        &spi_get_hw(SPI_PORT)->dr,  // Write to SPI data register
        framebuffer,                 // Read from framebuffer
        SCREEN_WIDTH * SCREEN_HEIGHT, // Number of transfers
        true                         // Start immediately
    );
}

void thumbycolor_clear(uint16_t color) {
    static uint16_t clear_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        clear_buffer[i] = color;
    }
    thumbycolor_update(clear_buffer);
    dma_channel_wait_for_finish_blocking(dma_channel);
}

// =============================================================================
// RGB LED
// =============================================================================

static uint led_r_slice, led_g_slice, led_b_slice;

static void led_init(void) {
    gpio_set_function(GPIO_LED_R, GPIO_FUNC_PWM);
    gpio_set_function(GPIO_LED_G, GPIO_FUNC_PWM);
    gpio_set_function(GPIO_LED_B, GPIO_FUNC_PWM);

    led_r_slice = pwm_gpio_to_slice_num(GPIO_LED_R);
    led_g_slice = pwm_gpio_to_slice_num(GPIO_LED_G);
    led_b_slice = pwm_gpio_to_slice_num(GPIO_LED_B);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 255);

    pwm_init(led_r_slice, &config, true);
    pwm_init(led_g_slice, &config, true);
    pwm_init(led_b_slice, &config, true);

    // Start with LED off
    thumbycolor_set_led(0, 0, 0);
}

void thumbycolor_set_led(uint8_t r, uint8_t g, uint8_t b) {
    pwm_set_gpio_level(GPIO_LED_R, r);
    pwm_set_gpio_level(GPIO_LED_G, g);
    pwm_set_gpio_level(GPIO_LED_B, b);
}

// =============================================================================
// Rumble Motor
// =============================================================================

static uint rumble_slice;

static void rumble_init(void) {
    gpio_set_function(GPIO_RUMBLE, GPIO_FUNC_PWM);
    rumble_slice = pwm_gpio_to_slice_num(GPIO_RUMBLE);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, 4095);  // 12-bit resolution
    pwm_init(rumble_slice, &config, true);

    thumbycolor_set_rumble(0);
}

void thumbycolor_set_rumble(uint8_t intensity) {
    // Map 0-255 to effective rumble range (2800-4095 for motor startup)
    uint16_t level = 0;
    if (intensity > 0) {
        level = 2800 + ((uint16_t)intensity * 1295 / 255);
    }
    pwm_set_gpio_level(GPIO_RUMBLE, level);
}

// =============================================================================
// Timing
// =============================================================================

uint32_t thumbycolor_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

void thumbycolor_wait_vsync(void) {
    // Simple frame timing (~60 FPS)
    static uint32_t last_frame = 0;
    uint32_t now = thumbycolor_time_ms();
    uint32_t elapsed = now - last_frame;

    if (elapsed < 16) {  // ~60 FPS
        sleep_ms(16 - elapsed);
    }
    last_frame = thumbycolor_time_ms();
}

// =============================================================================
// Audio System (PICO-8 Compatible)
// =============================================================================

#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_PWM_WRAP    255
#define AUDIO_NUM_CHANNELS 4

// PICO-8 frequency table (C-0 to D#-5, 64 notes)
// PICO-8 uses A4 = 440Hz
static const uint16_t p8_freq_table[64] = {
    65, 69, 73, 78, 82, 87, 92, 98,         // C-0 to G#-0
    104, 110, 117, 123, 131, 139, 147, 156, // A-0 to G#-1
    165, 175, 185, 196, 208, 220, 233, 247, // A-1 to G#-2
    262, 277, 294, 311, 330, 349, 370, 392, // A-2 to G#-3
    415, 440, 466, 494, 523, 554, 587, 622, // A-3 to G#-4
    659, 698, 740, 784, 831, 880, 932, 988, // A-4 to G#-5
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, // 48-55
    1661, 1760, 1865, 1976, 2093, 2217, 2349, 2489  // 56-63
};

// PICO-8 SFX data for Hyperspace
// Format: Speed(8), LoopStart(8), LoopEnd(8), followed by 32 notes
// Each note: pitch(6), waveform(3), volume(3), effect(3) - packed

typedef struct {
    uint8_t speed;
    uint8_t loop_start;
    uint8_t loop_end;
    uint8_t notes[32][4];  // pitch, waveform, volume, effect
} P8SFX;

// Hyperspace SFX definitions (extracted from PICO-8 cartridge)
static const P8SFX hyperspace_sfx[] = {
    // SFX 0: Laser fire (descending saw wave)
    {1, 0, 13, {
        {50, 2, 3, 0}, {51, 2, 3, 0}, {51, 2, 3, 0}, {49, 2, 1, 0},
        {46, 2, 3, 0}, {41, 2, 3, 0}, {36, 2, 4, 0}, {34, 2, 3, 0},
        {32, 2, 3, 0}, {29, 2, 3, 0}, {28, 2, 3, 0}, {28, 2, 2, 0},
        {28, 2, 1, 0}, {28, 2, 0, 0}, {28, 0, 0, 0}, {0, 0, 0, 0},
        {50, 4, 0, 0}, {52, 4, 0, 0}, {52, 4, 0, 0}, {49, 4, 0, 0},
        {46, 4, 0, 0}, {41, 4, 0, 0}, {36, 4, 0, 0}, {34, 4, 0, 0},
        {32, 4, 0, 0}, {29, 4, 0, 0}, {28, 4, 0, 0}, {28, 4, 0, 0},
        {28, 4, 0, 0}, {1, 4, 0, 0}, {1, 4, 0, 0}, {1, 4, 0, 0}
    }},
    // SFX 1: Player damage / barrel roll
    {5, 0, 0, {
        {36, 6, 7, 0}, {36, 6, 7, 0}, {39, 6, 7, 0}, {42, 6, 7, 0},
        {49, 6, 7, 0}, {56, 6, 7, 0}, {63, 6, 7, 0}, {63, 6, 7, 0},
        {48, 6, 7, 0}, {41, 6, 7, 0}, {36, 6, 7, 0}, {32, 6, 7, 0},
        {30, 6, 6, 0}, {28, 6, 6, 0}, {27, 6, 5, 0}, {26, 6, 5, 0},
        {25, 6, 4, 0}, {25, 6, 4, 0}, {24, 6, 3, 0}, {25, 6, 3, 0},
        {26, 6, 2, 0}, {28, 6, 2, 0}, {32, 6, 1, 0}, {35, 6, 1, 0},
        {10, 6, 0, 0}, {11, 6, 0, 0}, {13, 6, 0, 0}, {16, 6, 0, 0},
        {18, 6, 0, 0}, {20, 6, 0, 0}, {23, 6, 0, 0}, {24, 6, 0, 0}
    }},
    // SFX 2: Hit enemy / explosion
    {3, 0, 0, {
        {45, 6, 7, 0}, {41, 4, 7, 0}, {36, 4, 7, 0}, {25, 6, 7, 0},
        {30, 4, 7, 0}, {32, 6, 7, 0}, {29, 6, 7, 0}, {13, 6, 7, 0},
        {22, 6, 7, 0}, {20, 4, 7, 0}, {16, 4, 7, 0}, {15, 4, 7, 0},
        {19, 6, 7, 0}, {11, 4, 7, 0}, {9, 4, 7, 0}, {7, 6, 6, 0},
        {7, 4, 5, 0}, {5, 4, 4, 0}, {8, 6, 3, 0}, {2, 4, 2, 0},
        {1, 4, 1, 0}, {12, 6, 0, 0}, {5, 6, 0, 0}, {1, 6, 0, 0},
        {1, 6, 0, 0}, {1, 6, 0, 0}, {3, 6, 0, 0}, {1, 6, 0, 0},
        {2, 6, 0, 0}, {1, 6, 0, 0}, {1, 6, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 3: (unused placeholder)
    {1, 0, 0, {
        {60, 3, 7, 0}, {60, 0, 7, 0}, {55, 1, 7, 0}, {57, 0, 7, 0},
        {54, 0, 7, 0}, {51, 0, 7, 0}, {47, 1, 7, 0}, {48, 0, 7, 0},
        {41, 0, 7, 0}, {34, 0, 7, 0}, {32, 0, 7, 0}, {27, 0, 7, 0},
        {23, 0, 7, 0}, {29, 1, 7, 0}, {20, 0, 7, 0}, {19, 0, 7, 0},
        {18, 0, 7, 0}, {18, 0, 7, 0}, {19, 0, 7, 0}, {21, 0, 7, 0},
        {18, 1, 7, 0}, {23, 0, 7, 0}, {18, 1, 7, 0}, {30, 0, 7, 0},
        {39, 0, 7, 0}, {44, 0, 7, 0}, {53, 0, 7, 0}, {54, 0, 7, 0},
        {28, 1, 7, 0}, {33, 1, 7, 0}, {46, 1, 7, 0}, {0, 0, 0, 0}
    }},
    // SFX 4: (unused placeholder)
    {1, 0, 13, {
        {44, 4, 4, 0}, {18, 0, 4, 0}, {1, 0, 2, 0}, {16, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 5: Bonus pickup
    {1, 0, 0, {
        {44, 4, 7, 0}, {40, 4, 7, 0}, {35, 4, 7, 0}, {32, 4, 7, 0},
        {28, 4, 7, 0}, {26, 4, 7, 0}, {23, 4, 6, 0}, {21, 4, 4, 0},
        {21, 4, 2, 0}, {20, 4, 0, 0}, {22, 4, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 6: Boss spawn (eerie triangle wave)
    {24, 0, 0, {
        {0, 0, 0, 0}, {7, 3, 6, 0}, {20, 1, 4, 0}, {7, 3, 6, 0},
        {20, 1, 4, 0}, {26, 3, 7, 0}, {20, 1, 4, 0}, {27, 3, 7, 0},
        {1, 4, 4, 0}, {23, 3, 7, 0}, {23, 3, 7, 0}, {23, 3, 7, 0},
        {23, 3, 7, 0}, {23, 3, 6, 0}, {23, 3, 5, 0}, {23, 3, 0, 0},
        {1, 4, 0, 0}, {1, 4, 0, 0}, {23, 3, 0, 0}, {11, 4, 0, 0},
        {23, 0, 0, 0}, {23, 0, 0, 0}, {23, 0, 0, 0}, {23, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}
    }},
    // SFX 7: Boss damage
    {32, 0, 0, {
        {13, 2, 7, 0}, {13, 2, 7, 0}, {8, 2, 7, 0}, {8, 2, 7, 0},
        {4, 2, 7, 0}, {4, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0},
        {1, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0}, {1, 2, 7, 0},
        {18, 0, 0, 0}, {18, 0, 0, 0}, {18, 0, 0, 0}, {18, 0, 0, 0},
        {19, 0, 0, 0}, {20, 0, 0, 0}, {50, 0, 2, 0}, {20, 0, 0, 0},
        {20, 0, 0, 0}, {52, 0, 4, 0}, {68, 0, 4, 0}, {82, 0, 4, 0},
        {118, 0, 5, 0}, {82, 0, 4, 0}, {102, 0, 4, 0}, {82, 0, 4, 0},
        {82, 0, 4, 0}, {82, 0, 4, 0}, {1, 0, 4, 0}, {0, 0, 0, 0}
    }}
};

#define NUM_SFX (sizeof(hyperspace_sfx) / sizeof(hyperspace_sfx[0]))

// Audio channel state
typedef struct {
    const P8SFX *sfx;     // Current SFX being played
    int note_index;        // Current note index (0-31)
    int sample_count;      // Samples played for current note
    int samples_per_note;  // Samples per note (based on speed)
    uint32_t phase;        // Oscillator phase accumulator
    uint32_t phase_inc;    // Phase increment for frequency
    uint8_t volume;        // Current volume (0-7)
    uint8_t waveform;      // Current waveform (0-7)
    bool active;           // Channel is playing
    bool looping;          // SFX is looping
} AudioChannel;

static AudioChannel audio_channels[AUDIO_NUM_CHANNELS];
static uint audio_pwm_slice;
static uint8_t master_volume = 200;
static uint32_t lfsr = 0xACE1;  // For noise generation

// Waveform generators (return 0-255)
static inline uint8_t gen_triangle(uint32_t phase) {
    // phase is 0-65535
    uint16_t p = phase >> 8;
    if (p < 128) return p * 2;
    else return 255 - (p - 128) * 2;
}

static inline uint8_t gen_saw(uint32_t phase) {
    return phase >> 8;
}

static inline uint8_t gen_square(uint32_t phase, uint8_t duty) {
    uint16_t p = phase >> 8;
    return (p < duty) ? 255 : 0;
}

static inline uint8_t gen_noise(void) {
    // LFSR noise generator
    uint32_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1;
    lfsr = (lfsr >> 1) | (bit << 15);
    return (lfsr & 0xFF);
}

// Generate one audio sample (called at AUDIO_SAMPLE_RATE)
static uint8_t audio_generate_sample(void) {
    int32_t mix = 0;
    int active_count = 0;

    for (int ch = 0; ch < AUDIO_NUM_CHANNELS; ch++) {
        AudioChannel *c = &audio_channels[ch];
        if (!c->active || c->volume == 0) continue;

        uint8_t sample = 128;  // Center (silence)

        // Generate waveform based on type
        switch (c->waveform) {
            case 0: // Triangle
                sample = gen_triangle(c->phase);
                break;
            case 1: // Tilted saw (like triangle but asymmetric)
                sample = gen_saw(c->phase);
                break;
            case 2: // Saw
                sample = gen_saw(c->phase);
                break;
            case 3: // Square
                sample = gen_square(c->phase, 128);
                break;
            case 4: // Pulse (narrow)
                sample = gen_square(c->phase, 64);
                break;
            case 5: // Organ (square + square/2)
                sample = (gen_square(c->phase, 128) + gen_square(c->phase * 2, 128)) / 2;
                break;
            case 6: // Noise
                sample = gen_noise();
                break;
            case 7: // Phaser (two detuned saws)
                sample = (gen_saw(c->phase) + gen_saw(c->phase + 8192)) / 2;
                break;
        }

        // Apply volume (0-7 -> 0-255)
        int32_t vol_sample = ((int32_t)sample - 128) * c->volume / 7;
        mix += vol_sample;

        // Advance phase
        c->phase += c->phase_inc;

        active_count++;
    }

    // Mix and clamp
    if (active_count > 0) {
        mix = mix / active_count;  // Average channels
    }

    // Convert to unsigned and apply master volume
    int32_t out = 128 + (mix * master_volume / 255);
    if (out < 0) out = 0;
    if (out > 255) out = 255;

    return (uint8_t)out;
}

// Timer callback for audio generation
static volatile bool audio_timer_fired = false;
static struct repeating_timer audio_timer;

static bool audio_timer_callback(struct repeating_timer *t) {
    (void)t;
    uint8_t sample = audio_generate_sample();
    pwm_set_gpio_level(GPIO_AUDIO_PWM, sample);
    return true;
}

void thumbycolor_audio_init(void) {
    // Initialize audio enable pin
    gpio_init(GPIO_AUDIO_ENABLE);
    gpio_set_dir(GPIO_AUDIO_ENABLE, GPIO_OUT);
    gpio_put(GPIO_AUDIO_ENABLE, true);  // Enable audio

    // Initialize PWM for audio output
    gpio_set_function(GPIO_AUDIO_PWM, GPIO_FUNC_PWM);
    audio_pwm_slice = pwm_gpio_to_slice_num(GPIO_AUDIO_PWM);

    pwm_config config = pwm_get_default_config();
    pwm_config_set_wrap(&config, AUDIO_PWM_WRAP);
    pwm_config_set_clkdiv(&config, 1.0f);  // Full speed
    pwm_init(audio_pwm_slice, &config, true);

    // Start at center (silence)
    pwm_set_gpio_level(GPIO_AUDIO_PWM, 128);

    // Initialize channels
    for (int i = 0; i < AUDIO_NUM_CHANNELS; i++) {
        audio_channels[i].active = false;
        audio_channels[i].sfx = NULL;
    }

    // Start audio timer (~22kHz)
    add_repeating_timer_us(-45, audio_timer_callback, NULL, &audio_timer);  // ~22kHz
}

void thumbycolor_sfx(int n, int channel) {
    if (channel < 0 || channel >= AUDIO_NUM_CHANNELS) return;

    AudioChannel *c = &audio_channels[channel];

    if (n == -1) {
        // Stop this channel
        c->active = false;
        return;
    }

    if (n == -2) {
        // Stop all channels
        for (int i = 0; i < AUDIO_NUM_CHANNELS; i++) {
            audio_channels[i].active = false;
        }
        return;
    }

    if (n < 0 || n >= (int)NUM_SFX) return;

    // Start playing SFX
    c->sfx = &hyperspace_sfx[n];
    c->note_index = 0;
    c->sample_count = 0;
    c->phase = 0;

    // Calculate samples per note based on SFX speed
    // PICO-8: speed 1 = very fast, speed 255 = very slow
    // Each speed unit = 183 samples at 22050Hz (approximately)
    c->samples_per_note = c->sfx->speed * 183;
    if (c->samples_per_note < 183) c->samples_per_note = 183;

    // Set up first note
    uint8_t pitch = c->sfx->notes[0][0];
    c->waveform = c->sfx->notes[0][1];
    c->volume = c->sfx->notes[0][2];

    // Calculate phase increment for frequency
    if (pitch < 64 && c->volume > 0) {
        uint16_t freq = p8_freq_table[pitch];
        c->phase_inc = (freq * 65536) / AUDIO_SAMPLE_RATE;
        c->active = true;
    } else {
        c->active = false;
    }

    // Check if this SFX should loop
    c->looping = (c->sfx->loop_end > c->sfx->loop_start);
}

void thumbycolor_audio_update(void) {
    // Advance note positions for all active channels
    for (int ch = 0; ch < AUDIO_NUM_CHANNELS; ch++) {
        AudioChannel *c = &audio_channels[ch];
        if (!c->active || !c->sfx) continue;

        c->sample_count += AUDIO_SAMPLE_RATE / 60;  // Samples per frame at 60fps

        if (c->sample_count >= c->samples_per_note) {
            c->sample_count = 0;
            c->note_index++;

            // Check for loop or end
            if (c->looping && c->note_index >= c->sfx->loop_end) {
                c->note_index = c->sfx->loop_start;
            } else if (c->note_index >= 32) {
                c->active = false;
                continue;
            }

            // Update note parameters
            uint8_t pitch = c->sfx->notes[c->note_index][0];
            c->waveform = c->sfx->notes[c->note_index][1];
            c->volume = c->sfx->notes[c->note_index][2];

            if (pitch < 64 && c->volume > 0) {
                uint16_t freq = p8_freq_table[pitch];
                c->phase_inc = (freq * 65536) / AUDIO_SAMPLE_RATE;
            } else if (c->volume == 0) {
                // Note with 0 volume = rest, but don't stop channel
                c->phase_inc = 0;
            } else {
                c->active = false;
            }
        }
    }
}

void thumbycolor_set_volume(uint8_t volume) {
    master_volume = volume;
}

// =============================================================================
// Main Initialization
// =============================================================================

void thumbycolor_init(void) {
    // Note: stdio_init_all() should be called by main() before this

    // Initialize SPI
    spi_init(SPI_PORT, SPI_BAUDRATE_CMD);
    gpio_set_function(GPIO_SPI_TX, GPIO_FUNC_SPI);
    gpio_set_function(GPIO_SPI_CLK, GPIO_FUNC_SPI);

    // Initialize control pins
    gpio_init(GPIO_SPI_CS);
    gpio_init(GPIO_DC);
    gpio_init(GPIO_RST);

    gpio_set_dir(GPIO_SPI_CS, GPIO_OUT);
    gpio_set_dir(GPIO_DC, GPIO_OUT);
    gpio_set_dir(GPIO_RST, GPIO_OUT);

    gpio_put(GPIO_SPI_CS, true);   // Deselect
    gpio_put(GPIO_DC, true);       // Data mode
    gpio_put(GPIO_RST, true);      // Not in reset

    // Initialize subsystems
    backlight_init();
    dma_init_display();
    gc9107_init_sequence();
    buttons_init();
    led_init();
    rumble_init();
    thumbycolor_audio_init();  // Initialize audio

    // Set backlight to full
    thumbycolor_set_backlight(1.0f);
}
