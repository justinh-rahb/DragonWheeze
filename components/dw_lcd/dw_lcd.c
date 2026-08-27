// SPDX-License-Identifier: MIT
// dw_lcd — passive HT1621/TM1621 serial sniffer + display-RAM decoder.
//
// The mainboard MCU (HC32F005) writes the LCD over the TM1621C's 3-wire serial
// interface: CS (frame/chip-select, active low), WR (write clock, data valid on
// the rising edge), DATA (serial data). We only listen (all three pins are
// inputs) so there's no bus contention — nothing like the I2C/AHT20 E0.
//
// HT1621 frame (while CS low): a 3-bit mode tag, then payload —
//   100  Command mode  (config: SYS EN, LCD ON, bias/commons, …) — ignored here
//   101  WRITE mode     6-bit address (A5..A0, MSB first) then N x 4-bit data,
//                       address auto-increments each nibble until CS goes high
//   110  READ mode      (we never see the MCU read)
// The 32 x 4-bit write RAM is the display image; we mirror it and expose it.

#include "dw_lcd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include <string.h>

static const char *TAG = "dw_lcd";

static gpio_num_t s_cs, s_wr, s_data;

// ISR -> task byte ring (single producer/consumer). Values: 0/1 = data bit,
// 2 = frame start (CS fell), 3 = frame end (CS rose).
#define RING_SZ   4096            // power of two
#define B_ZERO    0
#define B_ONE     1
#define B_START   2
#define B_END     3
static volatile uint8_t  s_ring[RING_SZ];
static volatile uint32_t s_head, s_tail, s_overflows;

static inline void IRAM_ATTR ring_push(uint8_t v)
{
    uint32_t h = s_head;
    uint32_t n = (h + 1) & (RING_SZ - 1);
    if (n == s_tail) { s_overflows++; return; }   // full — drop, count it
    s_ring[h] = v;
    s_head = n;
}

static void IRAM_ATTR wr_isr(void *arg)
{
    (void)arg;
    if (gpio_get_level(s_cs) == 0)                 // only capture while selected
        ring_push(gpio_get_level(s_data) ? B_ONE : B_ZERO);
}

static void IRAM_ATTR cs_isr(void *arg)
{
    (void)arg;
    ring_push(gpio_get_level(s_cs) ? B_END : B_START);
}

// ---- task-side state ----
static uint8_t         s_ram[DW_LCD_RAM_ADDRS];
static dw_lcd_stats_t  s_stats;
static char            s_last_frame[192];
static size_t          s_last_frame_len;
static volatile bool   s_live;

static void decode_frame(const uint8_t *bits, size_t n)
{
    s_stats.frames++;
    s_stats.last_frame_bits = (uint32_t)n;

    // Keep the most recent raw frame as a bit string for diagnostics.
    size_t m = n < sizeof(s_last_frame) - 1 ? n : sizeof(s_last_frame) - 1;
    for (size_t i = 0; i < m; i++) s_last_frame[i] = bits[i] ? '1' : '0';
    s_last_frame[m] = '\0';
    s_last_frame_len = m;

    if (n < 3) return;
    uint8_t mode = (uint8_t)((bits[0] << 2) | (bits[1] << 1) | bits[2]);

    if (mode == 0b101) {                            // WRITE to display RAM
        if (n < 9) return;                          // need mode + 6-bit address
        uint8_t addr = 0;
        for (int i = 0; i < 6; i++) addr = (uint8_t)((addr << 1) | bits[3 + i]);
        size_t i = 9;
        while (i + 4 <= n) {                         // 4-bit data nibbles
            // Datasheet WRITE format: 101 a5..a0 d0 d1 d2 d3 — data is sent
            // d0 FIRST (LSB-first). Store so bit0 = d0 (RAM data D0..D3).
            uint8_t nib = 0;
            for (int b = 0; b < 4; b++) nib |= (uint8_t)(bits[i + b] << b);
            if (addr < DW_LCD_RAM_ADDRS) s_ram[addr] = nib;
            addr++;
            i += 4;
        }
        s_stats.writes++;
        s_live = true;
    } else if (mode == 0b100) {                      // command/config mode
        s_stats.commands++;
        s_live = true;
    }
}

// Gated capture: the TM1621 serial is busy enough that leaving the edge ISRs
// always-on starves WiFi on the single-core C3. Instead, open a short capture
// window every ~1s (the display changes slowly), so the average ISR load is a
// few percent and WiFi stays stable.
#define DW_LCD_CAPTURE_MS   60      // interrupts ON — snapshot the bus
#define DW_LCD_PERIOD_MS  1000      // one snapshot per second
#define DW_LCD_STARTUP_MS 6000      // let WiFi connect before we ever enable ISRs

static void lcd_task(void *arg)
{
    (void)arg;
    static uint8_t frame[320];

    vTaskDelay(pdMS_TO_TICKS(DW_LCD_STARTUP_MS));   // WiFi first

    for (;;) {
        // Fresh window: drop any stale ring bytes, then let the ISRs run briefly.
        s_tail = s_head;
        gpio_intr_enable(s_wr);
        gpio_intr_enable(s_cs);
        vTaskDelay(pdMS_TO_TICKS(DW_LCD_CAPTURE_MS));
        gpio_intr_disable(s_wr);
        gpio_intr_disable(s_cs);

        // Decode everything captured this window into the RAM shadow.
        size_t fn = 0;
        bool in_frame = false;
        while (s_tail != s_head) {
            uint8_t v = s_ring[s_tail];
            s_tail = (s_tail + 1) & (RING_SZ - 1);
            switch (v) {
            case B_START: fn = 0; in_frame = true; break;
            case B_END:   if (in_frame) decode_frame(frame, fn); in_frame = false; break;
            default:      if (in_frame && fn < sizeof(frame)) frame[fn++] = v; break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DW_LCD_PERIOD_MS - DW_LCD_CAPTURE_MS));
    }
}

esp_err_t dw_lcd_init(gpio_num_t cs, gpio_num_t wr, gpio_num_t data)
{
    s_cs = cs; s_wr = wr; s_data = data;

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << cs) | (1ULL << wr) | (1ULL << data),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // lines are actively driven by the MCU
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&in);
    if (e != ESP_OK) return e;

    gpio_set_intr_type(wr, GPIO_INTR_POSEDGE);  // sample DATA on WR rising edge
    gpio_set_intr_type(cs, GPIO_INTR_ANYEDGE);  // frame start/end

    e = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;   // ok if already installed
    gpio_isr_handler_add(wr, wr_isr, NULL);
    gpio_isr_handler_add(cs, cs_isr, NULL);
    gpio_intr_disable(wr);          // stay OFF until the gated task opens a window
    gpio_intr_disable(cs);

    xTaskCreate(lcd_task, "dw_lcd", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "HT1621 sniffer up: CS=%d WR=%d DATA=%d", (int)cs, (int)wr, (int)data);
    return ESP_OK;
}

void dw_lcd_get_ram(uint8_t out[DW_LCD_RAM_ADDRS]) { memcpy(out, s_ram, DW_LCD_RAM_ADDRS); }

void dw_lcd_get_stats(dw_lcd_stats_t *out)
{
    if (!out) return;
    *out = s_stats;
    out->ring_overflows = s_overflows;
}

size_t dw_lcd_get_last_frame_bits(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return 0;
    size_t n = s_last_frame_len < out_sz - 1 ? s_last_frame_len : out_sz - 1;
    memcpy(out, s_last_frame, n);
    out[n] = '\0';
    return n;
}

bool dw_lcd_is_live(void) { return s_live; }
