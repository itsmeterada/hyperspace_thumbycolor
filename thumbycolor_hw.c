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
#include <string.h>

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
#define DISPLAY_OFFSET_X 2
#define DISPLAY_OFFSET_Y 1

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

    // Use 8-bit SPI for initialization
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(SPI_PORT, SPI_BAUDRATE_CMD);

    // Inter-register enable commands (from TinyCircuits)
    gc9107_cmd_with_data(0xFE, NULL, 0);
    gc9107_cmd_with_data(0xEF, NULL, 0);

    // Sleep out
    gc9107_cmd_with_data(GC9107_SLPOUT, NULL, 0);
    sleep_ms(120);

    // Memory Data Access Control
    // Bit 7 (MY): Row address order (flip vertical)
    // Bit 6 (MX): Column address order (flip horizontal)
    uint8_t madctl = 0xC0;  // 180° rotation
    gc9107_cmd_with_data(GC9107_MADCTL, &madctl, 1);

    // Interface Pixel Format: 16-bit RGB565
    uint8_t colmod = 0x05;  // 16-bit RGB565
    gc9107_cmd_with_data(GC9107_COLMOD, &colmod, 1);

    // RGB565 complement setting (from TinyCircuits)
    gc9107_cmd_with_data(0xAC, (uint8_t[]){0xC8}, 1);

    // GC9107 specific registers (from TinyCircuits)
    gc9107_cmd_with_data(0xB0, (uint8_t[]){0xC0}, 1);
    gc9107_cmd_with_data(0xB1, (uint8_t[]){0x80}, 1);
    gc9107_cmd_with_data(0xB2, (uint8_t[]){0x3d}, 1);
    gc9107_cmd_with_data(0xB3, (uint8_t[]){0x1f}, 1);
    gc9107_cmd_with_data(0xB7, (uint8_t[]){0x35}, 1);

    gc9107_cmd_with_data(0xBB, (uint8_t[]){0x39}, 1);
    gc9107_cmd_with_data(0xC0, (uint8_t[]){0x2c}, 1);
    gc9107_cmd_with_data(0xC2, (uint8_t[]){0x01}, 1);
    gc9107_cmd_with_data(0xC3, (uint8_t[]){0x17}, 1);
    gc9107_cmd_with_data(0xC4, (uint8_t[]){0x20}, 1);
    gc9107_cmd_with_data(0xC6, (uint8_t[]){0x0f}, 1);  // Frame rate
    gc9107_cmd_with_data(0xAB, (uint8_t[]){0x0f}, 1);

    gc9107_cmd_with_data(0xD0, (uint8_t[]){0xA4, 0xA1}, 2);

    // Positive voltage gamma control (from TinyCircuits)
    gc9107_cmd_with_data(0xF0, (uint8_t[]){
        0x45, 0x09, 0x08, 0x08, 0x26, 0x2A
    }, 6);

    // Negative voltage gamma control (from TinyCircuits)
    gc9107_cmd_with_data(0xF1, (uint8_t[]){
        0x43, 0x70, 0x72, 0x36, 0x37, 0x6F
    }, 6);

    // Set display window to full screen
    gc9107_set_window(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    // Display Inversion ON (0x21)
    gc9107_cmd_with_data(0x21, NULL, 0);

    // Display ON
    gc9107_cmd_with_data(GC9107_DISPON, NULL, 0);
    sleep_ms(10);

    // Switch to 16-bit SPI for pixel data
    spi_set_format(SPI_PORT, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
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

    // Switch to 8-bit for command
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Set window to full screen
    gc9107_set_window(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    // Start memory write
    gc9107_write_cmd(GC9107_RAMWR);

    // Switch to 16-bit for pixel data
    spi_set_format(SPI_PORT, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Start DMA transfer
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

    // Set backlight to full
    thumbycolor_set_backlight(1.0f);
}
