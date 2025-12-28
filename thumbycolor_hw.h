/*
 * ThumbyColor Hardware Abstraction Layer
 * Based on TinyCircuits Tiny Game Engine GPIO definitions
 *
 * Hardware: RP2350 + GC9107 128x128 LCD
 */

#ifndef THUMBYCOLOR_HW_H
#define THUMBYCOLOR_HW_H

#include <stdint.h>
#include <stdbool.h>

// Screen dimensions
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 128

// Color type (RGB565)
typedef uint16_t color_t;

// =============================================================================
// GPIO Pin Definitions (from TinyCircuits Tiny Game Engine)
// =============================================================================

// D-Pad buttons (directly mapped)
#define GPIO_BUTTON_DPAD_UP     1
#define GPIO_BUTTON_DPAD_LEFT   0
#define GPIO_BUTTON_DPAD_DOWN   3
#define GPIO_BUTTON_DPAD_RIGHT  2

// Action buttons
#define GPIO_BUTTON_A           21
#define GPIO_BUTTON_B           25

// Shoulder buttons
#define GPIO_BUTTON_BUMPER_LEFT  6
#define GPIO_BUTTON_BUMPER_RIGHT 22

// Menu button
#define GPIO_BUTTON_MENU        26

// Display SPI pins (directly mapped to SPI0)
#define GPIO_SPI_TX             19   // MOSI (SDA)
#define GPIO_SPI_CLK            18   // SCK
#define GPIO_SPI_CS             17   // Chip Select
#define GPIO_DC                 16   // Data/Command
#define GPIO_RST                4    // Reset
#define GPIO_BACKLIGHT          7    // Backlight PWM

// Audio
#define GPIO_AUDIO_PWM          23
#define GPIO_AUDIO_ENABLE       20

// Battery & Charging
#define GPIO_CHARGE_STAT        24
#define GPIO_BATTERY_ADC        29

// RGB LED
#define GPIO_LED_R              11
#define GPIO_LED_G              10
#define GPIO_LED_B              12

// Rumble motor
#define GPIO_RUMBLE             5

// =============================================================================
// Button Codes (bitmask)
// =============================================================================

#define BUTTON_UP       (1 << 0)
#define BUTTON_DOWN     (1 << 1)
#define BUTTON_LEFT     (1 << 2)
#define BUTTON_RIGHT    (1 << 3)
#define BUTTON_A        (1 << 4)
#define BUTTON_B        (1 << 5)
#define BUTTON_BUMPER_L (1 << 6)
#define BUTTON_BUMPER_R (1 << 7)
#define BUTTON_MENU     (1 << 8)

// =============================================================================
// Function Declarations
// =============================================================================

// Initialization
void thumbycolor_init(void);

// Display
void thumbycolor_set_backlight(float brightness);  // 0.0 - 1.0
void thumbycolor_update(uint16_t *framebuffer);
void thumbycolor_clear(uint16_t color);

// Input
uint32_t thumbycolor_get_buttons(void);
bool thumbycolor_button_pressed(uint32_t button);

// Timing
uint32_t thumbycolor_time_ms(void);
void thumbycolor_wait_vsync(void);

// Utility
void thumbycolor_set_led(uint8_t r, uint8_t g, uint8_t b);
void thumbycolor_set_rumble(uint8_t intensity);

// RGB565 color conversion
static inline uint16_t thumbycolor_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

#endif // THUMBYCOLOR_HW_H
