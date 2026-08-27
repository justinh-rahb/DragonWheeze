// SPDX-License-Identifier: MIT
// dw_lcd — passive sniffer/decoder for the SH01 mainboard's TM1621C (HT1621-
// compatible) LCD driver. We do NOT drive anything: three GPIOs listen on the
// 3-wire serial link between the mainboard MCU (HC32F005) and the TM1621C
// (CS / WR / DATA), decode the HT1621 write commands, and mirror the driver's
// 32x4-bit display RAM. That reconstructs exactly what's on the LCD — the
// digits, the mode/status icons, and the ambient temp/humidity the dryer shows
// (E0 too) — with zero bus contention (unlike the I2C AHT20 path → no E0).
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HT1621 display RAM: 32 addresses x 4 bits = 128 segments.
#define DW_LCD_RAM_ADDRS 32

typedef struct {
    uint32_t frames;         // CS-framed transactions seen
    uint32_t writes;         // decoded WRITE (RAM) commands
    uint32_t commands;       // decoded command-mode frames (config)
    uint32_t ring_overflows; // ISR ring dropped bytes (sniff too slow) — should stay 0
    uint32_t last_frame_bits;// bit count of the most recent frame
} dw_lcd_stats_t;

// Start the passive sniffer on the three tap pins (all configured as inputs).
esp_err_t dw_lcd_init(gpio_num_t cs, gpio_num_t wr, gpio_num_t data);

// Copy the current 32-nibble RAM shadow (low nibble of each byte = 4 segments).
void dw_lcd_get_ram(uint8_t out[DW_LCD_RAM_ADDRS]);

void dw_lcd_get_stats(dw_lcd_stats_t *out);

// Diagnostic: fetch the most recent raw frame as a '0'/'1' bit string (MSB =
// first bit after CS went low). Returns the number of bits, 0 if none yet.
size_t dw_lcd_get_last_frame_bits(char *out, size_t out_sz);

// Whether any traffic has been decoded (tap wired & MCU talking).
bool dw_lcd_is_live(void);

#ifdef __cplusplus
}
#endif
