// WardriveESP — color status screens: the Koko Marauder V8's 240x320 ILI9341 and the Marauder Mini v3's 128x128 ST7735
// over SPI, and the Waveshare ESP32-S3 Touch AMOLED 2.06's 410x502 CO5300 over QSPI. Boot splash, live dashboard,
// alerts (scenes in lcd_draw.c); on the watch, four touch pages. Drawn in 8-row strips into one small DMA buffer; a
// strip goes to the panel only when its pixels changed.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "lcd.h"
#include "lcd_draw.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include "nvs.h"
#include "gesture.h"
#endif

static const char *TAG = "WardriveESP";

// Both boards (ESP32-C5) share one SPI bus layout: panel, microSD (and the Koko's XPT2046 touch) on 6 / 2 / 7.
#define PIN_MISO     2
#define PIN_MOSI     7
#define PIN_SCLK     6
#define PIN_CS       23
#define PIN_DC       24
#define PIN_TOUCH_CS 9     // Koko only
#define PIN_SD_CS    10
#define HOST         SPI2_HOST
#define CLK_READ_HZ  1000000
#define CLK_WRITE_HZ 20000000

#define SH LCD_SH
#define FRAME_MS     200
#define ALERT_BLINK_MS 250
#define BL_ON   255   // always full: an offline board is still working, and the screen is how you tell
#define BL_LEVEL(pct) ((pct) * BL_ON / 100)   // the watch's brightness setting (every other screen stays at 100 %)
// Touch screens (the watch): its loop and its drawing pace.
#define TOUCH_POLL_MS 20                   // touch polling while the panel is lit (and between strips of a frame)
#define DARK_POLL_MS  50                   // while it is dark: the wake touch and the BOOT button
#define HOLD_FRAME_MS 50                   // redraws while a button fills under a held finger
#define DIAG_MS       60000                // the screen's own numbers in the log this often
#define ANIM_STEP_MS  1000                 // the watch's idle animations (pulses, scan dots) step once a second
#define SPLASH_FAST_MS 2500                // the boot splash's radar sweeps smoothly (it shows for 2.2 s)

typedef struct { uint8_t c, n; uint8_t p[16]; uint16_t ms; } init_t;

// ILI9341 power-up (no reset line: software reset), 16-bit color, portrait with BGR order.
__attribute__((unused)) static const init_t INIT_ILI9341[] = {
    { 0x01, 0, {0}, 150 },
    { 0xCF, 3, { 0x00, 0xC1, 0x30 }, 0 },
    { 0xED, 4, { 0x64, 0x03, 0x12, 0x81 }, 0 },
    { 0xE8, 3, { 0x85, 0x00, 0x78 }, 0 },
    { 0xCB, 5, { 0x39, 0x2C, 0x00, 0x34, 0x02 }, 0 },
    { 0xF7, 1, { 0x20 }, 0 },
    { 0xEA, 2, { 0x00, 0x00 }, 0 },
    { 0xC0, 1, { 0x23 }, 0 },             // power control 1
    { 0xC1, 1, { 0x10 }, 0 },             // power control 2
    { 0xC5, 2, { 0x3E, 0x28 }, 0 },       // VCOM 1
    { 0xC7, 1, { 0x86 }, 0 },             // VCOM 2
    { 0x36, 1, { 0x48 }, 0 },             // MADCTL: MX + BGR (portrait)
    { 0x3A, 1, { 0x55 }, 0 },             // 16 bits per pixel
    { 0xB1, 2, { 0x00, 0x18 }, 0 },       // frame rate
    { 0xB6, 3, { 0x08, 0x82, 0x27 }, 0 }, // display function
    { 0xF2, 1, { 0x00 }, 0 },
    { 0x26, 1, { 0x01 }, 0 },             // gamma curve 1
    { 0xE0, 15, { 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00 }, 0 },
    { 0xE1, 15, { 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F }, 0 },
    { 0x20, 0, {0}, 0 },                  // inversion off
    { 0x11, 0, {0}, 120 },                // sleep out
};

// ST7735 (green tab, 128x128) power-up, the same register values the board's stock firmware sends.
__attribute__((unused)) static const init_t INIT_ST7735[] = {
    { 0x01, 0, {0}, 150 },                // software reset
    { 0x11, 0, {0}, 255 },                // sleep out
    { 0xB1, 3, { 0x01, 0x2C, 0x2D }, 0 }, // frame rate: normal
    { 0xB2, 3, { 0x01, 0x2C, 0x2D }, 0 }, // idle
    { 0xB3, 6, { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D }, 0 },   // partial
    { 0xB4, 1, { 0x07 }, 0 },             // no inversion
    { 0xC0, 3, { 0xA2, 0x02, 0x84 }, 0 }, // power control 1-5
    { 0xC1, 1, { 0xC5 }, 0 },
    { 0xC2, 2, { 0x0A, 0x00 }, 0 },
    { 0xC3, 2, { 0x8A, 0x2A }, 0 },
    { 0xC4, 2, { 0x8A, 0xEE }, 0 },
    { 0xC5, 1, { 0x0E }, 0 },             // VCOM
    { 0x20, 0, {0}, 0 },                  // inversion off
    { 0x36, 1, { 0xC8 }, 0 },             // MADCTL: MX + MY + BGR
    { 0x3A, 1, { 0x05 }, 0 },             // 16 bits per pixel
    { 0xE0, 16, { 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 }, 0 },
    { 0xE1, 16, { 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D, 0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 }, 0 },
    { 0x13, 0, {0}, 10 },                 // normal display on
};

typedef struct {
    const char *name;
    int w, h, xofs, yofs;                 // drawn size and where it sits in the controller's memory
    int bl;                               // backlight GPIO, -1: brightness is the panel's own (DCS 51h, AMOLED)
    bool bl_low;                          // backlight on when the pin is low
    const init_t *init;
    size_t n_init;
    bool qspi;                            // QSPI panel: commands and pixels framed by esp_lcd's QSPI panel IO, no D/C pin
} panel_t;

__attribute__((unused)) static const panel_t KOKO = { "240x320 ILI9341", 240, 320, 0, 0, 8, false, INIT_ILI9341,
                              sizeof(INIT_ILI9341) / sizeof(INIT_ILI9341[0]), false };
__attribute__((unused)) static const panel_t MINI = { "128x128 ST7735", 128, 128, 2, 3, 5, true, INIT_ST7735,
                              sizeof(INIT_ST7735) / sizeof(INIT_ST7735[0]), false };

#if CONFIG_IDF_TARGET_ESP32S3
// Waveshare ESP32-S3 Touch AMOLED 2.06: CO5300 AMOLED, 410x502, QSPI on SPI2 (Waveshare BSP esp32_s3_touch_amoled_2_06.h
// and board_config/pin_config.h). Columns start 22 into the controller's memory (esp_lcd_panel_set_gap(panel, 0x16, 0)).
#define AM_PIN_SCLK   11
#define AM_PIN_CS     12
#define AM_PIN_D0     4
#define AM_PIN_D1     5
#define AM_PIN_D2     6
#define AM_PIN_D3     7
#define AM_PIN_RST    8
#define AM_HOST       SPI2_HOST
#define AM_CLK_HZ     (40 * 1000 * 1000)   // esp_lcd_sh8601 2.0.0 SH8601_PANEL_IO_QSPI_CONFIG
#define AM_W          410
#define AM_H          502
#define AM_XGAP       22
// QSPI framing (esp_lcd_sh8601.c): a register write is opcode 02h then 00 <cmd> 00 on one line; pixels are opcode 32h
// then 00 2Ch 00, the data on four lines.
#define AM_OP_CMD     0x02u
#define AM_OP_PIXELS  0x32u

// Power-up: the reset pin first (lcd_open_amoled), then MADCTL RGB and 16-bit color (what esp_lcd_sh8601 sends before a
// vendor table), then Waveshare's table (BSP lcd_init_cmds) with brightness 0. Display on (29h) comes in lcd_start once
// the splash is in the panel's memory, so the AMOLED never shows what the reset left there.
static const init_t INIT_CO5300[] = {
    { 0x36, 1, { 0x00 }, 0 },                     // MADCTL: RGB
    { 0x3A, 1, { 0x55 }, 0 },                     // 16 bits per pixel
    { 0x11, 0, {0}, 120 },                        // sleep out
    { 0xC4, 1, { 0x80 }, 0 },
    { 0x44, 2, { 0x01, 0xD1 }, 0 },
    { 0x35, 1, { 0x00 }, 0 },                     // tearing-effect line on
    { 0x53, 1, { 0x20 }, 10 },                    // brightness control on
    { 0x63, 1, { 0xFF }, 10 },
    { 0x51, 1, { 0x00 }, 10 },                    // brightness 0 until the first frame is in
    { 0x2A, 4, { 0x00, AM_XGAP, (AM_XGAP + AM_W - 1) >> 8, (AM_XGAP + AM_W - 1) & 0xFF }, 0 },
    { 0x2B, 4, { 0x00, 0x00, (AM_H - 1) >> 8, (AM_H - 1) & 0xFF }, 0 },
};
static const panel_t AMOLED = { "410x502 CO5300", AM_W, AM_H, AM_XGAP, 0, -1, false, INIT_CO5300,
                                sizeof(INIT_CO5300) / sizeof(INIT_CO5300[0]), true };
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_io_done;   // the pixels of the last strip are out: the strip buffer can be drawn again

// The watch's screen settings (controls page), kept in NVS. Both step through these on each press.
static const uint16_t BRIGHT_STEPS[] = { 25, 50, 75, 100 };   // percent
static const uint16_t SLEEP_STEPS[]  = { 15, 30, 60, 120 };   // seconds without a touch before the panel goes dark
#define NVS_NS        "iosesp"
#define NVS_BRIGHT    "scrbright"
#define NVS_SLEEP     "scrsleep"
static lcd_scan_fn s_scan;
static gest_t s_gest;
static bool s_wake_finger;            // the finger that woke the panel: ignored until it lifts
static uint8_t s_page, s_net_first;
static int8_t s_hold_btn = LCD_BTN_NONE;
static uint8_t s_hold_pct;
#endif
static uint8_t  s_bright_pct = 100;   // the watch's brightness setting (every other screen stays at full)
static uint16_t s_sleep_s = 30;       // the watch's screen-off timeout

static const panel_t *s_panel;
static spi_device_handle_t s_dev;
static bool s_bus;
static bool s_sd_cs;                 // the microSD slot's chip select read pulled up when the bus opened
static SemaphoreHandle_t s_bus_lock; // the panel and the microSD card take turns on the bus (sdrun.c)
static uint16_t *s_strip;            // SH rows of pixels, byte-swapped for the wire
static uint32_t s_hash[LCD_STRIPS];  // what each strip on the panel holds now
static TaskHandle_t s_task;
static lcd_fill_fn s_fill;
static const char *s_board = "", *s_version = "";
static volatile bool s_stealth;
static volatile uint32_t s_alert_since, s_alert_until;
static char s_alert_label[13];
static volatile uint32_t s_alert_rgb;
static volatile bool s_alert_police;
static uint32_t s_t0;
static lcd_touch_fn s_touch;        // touch screens (the watch): finger down and where
static lcd_frame_t s_frame;         // the frame being drawn (static: the watch's carries its recent networks, ~1.6 KB)
static bool s_down, s_dark;                 // the finger now, the panel dark
static volatile bool s_redraw;              // a touch or a new alert changed what the frame shows: draw it now
// The watch's drawing cost over its last DIAG_MS window, for the bench diagnostics (lcd_diag_last).
static volatile uint16_t s_diag_frames, s_diag_avg_ms, s_diag_worst_ms;
static volatile bool s_diag_log = true;     // the "lcd:" log line (off: a stack mark without that call in it)
static int s_tx, s_ty;
static uint32_t s_poll_at, s_touch_at;      // last touch poll, last time a finger was down
static void touch_tick(uint32_t now);

// Channel activity from AP sightings (14 × 2.4 GHz, then 28 × 5 GHz). The counts themselves come from apcount via the
// fill callback; "+N new" is the session count now minus ten seconds ago.
static volatile uint8_t s_act[LCD_ACT_N];
static uint32_t s_session_at[10];   // session count at the start of each of the last ten seconds

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── SPI / QSPI ──
#if CONFIG_IDF_TARGET_ESP32S3
#define AM_DONE_MS 100   // one strip is ~8 KB, under a millisecond at 40 MHz on four lines

static bool IRAM_ATTR am_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx) {
    (void)io; (void)ev; (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_io_done, &woken);
    return woken == pdTRUE;
}

// The pixels go out by DMA after esp_lcd queues them: wait, so the strip buffer isn't drawn over mid-transfer.
static esp_err_t am_pixels(const void *buf, size_t n) {
    esp_err_t e = esp_lcd_panel_io_tx_color(s_io, (int)(AM_OP_PIXELS << 24 | 0x2Cu << 8), buf, n);
    if (e == ESP_OK && xSemaphoreTake(s_io_done, pdMS_TO_TICKS(AM_DONE_MS)) != pdTRUE) e = ESP_ERR_TIMEOUT;
    return e;
}
#endif

// Pixels (`data`) or a command byte. On QSPI only pixels come through here: cmd() frames its own.
static esp_err_t send(bool data, const void *buf, size_t n) {
    if (!n) return ESP_OK;
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_panel->qspi) return am_pixels(buf, n);
#endif
    gpio_set_level(PIN_DC, data);
    spi_transaction_t t = { .length = n * 8, .tx_buffer = buf };
    return spi_device_polling_transmit(s_dev, &t);
}
static esp_err_t cmd(uint8_t c, const uint8_t *p, size_t n) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_panel->qspi) return esp_lcd_panel_io_tx_param(s_io, (int)(AM_OP_CMD << 24 | (uint32_t)c << 8), p, n);
#endif
    esp_err_t e = send(false, &c, 1);
    return e == ESP_OK ? send(true, p, n) : e;
}

// Command `c`, then `n` (≤ 4) bytes back with CS held low across both. On a 3-wire device the answer comes back on MOSI.
__attribute__((unused)) static esp_err_t read_reg(uint8_t c, uint8_t *out, size_t n, bool three_wire) {
    if (spi_device_acquire_bus(s_dev, portMAX_DELAY) != ESP_OK) return ESP_FAIL;
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = { .length = 8, .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE };
    t.tx_data[0] = c;
    esp_err_t e = spi_device_polling_transmit(s_dev, &t);
    if (e == ESP_OK) {
        gpio_set_level(PIN_DC, 1);
        spi_transaction_t r = { .length = three_wire ? 0 : n * 8, .rxlength = n * 8,
                                .flags = SPI_TRANS_USE_RXDATA | (three_wire ? 0 : SPI_TRANS_USE_TXDATA) };
        e = spi_device_polling_transmit(s_dev, &r);
        memcpy(out, r.rx_data, n);
    }
    spi_device_release_bus(s_dev);
    return e;
}

__attribute__((unused)) static esp_err_t add_device(int hz, bool three_wire) {
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    spi_device_interface_config_t dc = { .clock_speed_hz = hz, .mode = 0, .spics_io_num = PIN_CS, .queue_size = 1,
                                         .flags = three_wire ? SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX : 0 };
    return spi_bus_add_device(HOST, &dc, &s_dev);
}

__attribute__((unused)) static void bus_release(void) {
    if (s_dev) { spi_bus_remove_device(s_dev); s_dev = NULL; }
    if (s_bus) { spi_bus_free(HOST); s_bus = false; }
    s_sd_cs = false;
    static const int PINS[] = { PIN_MISO, PIN_MOSI, PIN_SCLK, PIN_CS, PIN_DC, PIN_TOUCH_CS, PIN_SD_CS };
    for (size_t i = 0; i < sizeof(PINS) / sizeof(PINS[0]); i++) gpio_reset_pin(PINS[i]);
}

// Input only, internal pull-down: a pin that still reads high has an external pull-up (a chip select's idle state).
__attribute__((unused)) static bool pulled_up(int gpio) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(200);
    int high = 0;
    for (int i = 0; i < 5; i++) { high += gpio_get_level(gpio); esp_rom_delay_us(50); }
    gpio_reset_pin(gpio);
    return high >= 4;
}

// Only when one of the bus's chip selects already reads pulled up: never drive these pins on a board without them.
// Opens the bus and holds every pulled-up select other than the panel's at its resting (deselected) level.
__attribute__((unused)) static bool bus_open(const char *who, bool touch_too) {
    bool cs = pulled_up(PIN_CS), tcs = touch_too && pulled_up(PIN_TOUCH_CS), sdcs = pulled_up(PIN_SD_CS);
    ESP_LOGI(TAG, "%s probe: pulled up CS %d=%d, touch CS %d=%d, SD CS %d=%d", who, PIN_CS, cs, PIN_TOUCH_CS, tcs,
             PIN_SD_CS, sdcs);
    if (!cs && !tcs && !sdcs) return false;
    if (tcs) { gpio_set_direction(PIN_TOUCH_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_TOUCH_CS, 1); }
    if (sdcs) { gpio_set_direction(PIN_SD_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_SD_CS, 1); }
    s_sd_cs = sdcs;
    if (sdcs) {
        // A microSD card that the last boot put in SPI mode (a warm reset never powers it down) can keep driving MISO
        // and garble the panel's ID read. Clocks with every select high make it let go of the line.
        gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT); gpio_set_level(PIN_CS, 1);
        gpio_set_direction(PIN_MOSI, GPIO_MODE_OUTPUT); gpio_set_level(PIN_MOSI, 1);
        gpio_set_direction(PIN_SCLK, GPIO_MODE_OUTPUT);
        for (int i = 0; i < 80; i++) {
            gpio_set_level(PIN_SCLK, 1); esp_rom_delay_us(4);
            gpio_set_level(PIN_SCLK, 0); esp_rom_delay_us(4);
        }
    }
    spi_bus_config_t bc = { .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCLK,
                            .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = LCD_W * SH * 2 + 16 };
    if (spi_bus_initialize(HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) { bus_release(); return false; }
    s_bus = true;
    gpio_set_direction(PIN_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DC, 1);
    return true;
}

__attribute__((unused)) static bool is_ili9341(const uint8_t *id) { return id[1] == 0x93 && id[2] == 0x41; }

bool lcd_probe_koko(void) {
#if !CONFIG_IDF_TARGET_ESP32C5
    return false;
#else
    if (s_bus) return s_panel == &KOKO;
    if (!bus_open("koko lcd", true)) return false;
    if (add_device(CLK_READ_HZ, false) != ESP_OK) { bus_release(); return false; }
    uint8_t id[4] = {0};
    read_reg(0xD3, id, 4, false);   // ID4: dummy, 00, 93, 41
    bool ok = is_ili9341(id + 1) || is_ili9341(id);
    if (!ok) {
        // Panels that only answer ID4 one byte at a time, indexed through the undocumented 0xD9 register.
        uint8_t b[3] = {0};
        for (int i = 0; i < 3; i++) {
            const uint8_t idx = (uint8_t)(0x10 + i + 1);
            cmd(0xD9, &idx, 1);
            uint8_t v = 0;
            read_reg(0xD3, &v, 1, false);
            b[i] = v;
        }
        ESP_LOGI(TAG, "koko lcd probe: ID4 indexed %02x %02x %02x", b[0], b[1], b[2]);
        ok = is_ili9341(b);
    }
    ESP_LOGI(TAG, "koko lcd probe: ID4 %02x %02x %02x %02x -> %s", id[0], id[1], id[2], id[3], ok ? "ILI9341" : "none");
    if (!ok || add_device(CLK_WRITE_HZ, false) != ESP_OK) { bus_release(); return false; }
    s_panel = &KOKO;
    return true;
#endif
}

bool lcd_probe_mini(bool stored) {
#if !CONFIG_IDF_TARGET_ESP32C5
    (void)stored;
    return false;
#else
    if (s_bus) return s_panel == &MINI;
    if (!bus_open("mini lcd", false)) return false;
    // The ST7735 answers only on its one data line: 3-wire on the bus's MOSI. RDID1 (DAh) is one byte with no dummy
    // clock; RDDID (04h) is one dummy clock, then 24 bits. Some modules also bring it out on MISO (4-wire).
    uint8_t r[4] = {0};
    bool ok = false;
    if (add_device(CLK_READ_HZ / 2, true) == ESP_OK) {
        read_reg(0xDA, r, 1, true);
        ok = r[0] == 0x7C;
        ESP_LOGI(TAG, "mini lcd probe: RDID1 3-wire %02x", r[0]);
        if (!ok) {
            read_reg(0x04, r, 4, true);
            uint32_t raw = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
            ok = ((raw >> 23) & 0xFF) == 0x7C;
            ESP_LOGI(TAG, "mini lcd probe: RDDID 3-wire raw %08lx", (unsigned long)raw);
        }
    }
    if (!ok && add_device(CLK_READ_HZ / 2, false) == ESP_OK) {
        read_reg(0xDA, r, 1, false);
        ok = r[0] == 0x7C;
        ESP_LOGI(TAG, "mini lcd probe: RDID1 4-wire %02x", r[0]);
    }
    // A panel that can't be read back still counts when the board was told it is a Mini (`board marauder_mini_v3`).
    ESP_LOGI(TAG, "mini lcd probe: %s", ok ? "ST7735" : stored ? "no ID read, stored identity says Mini" : "none");
    if ((!ok && !stored) || add_device(CLK_WRITE_HZ, false) != ESP_OK) { bus_release(); return false; }
    s_panel = &MINI;
    return true;
#endif
}

// Waveshare ESP32-S3 Touch AMOLED 2.06: only called once the board was identified (its AXP2101 and touch controller
// answered on the board bus), so these pins are known to be the panel's. Reset, QSPI bus, esp_lcd panel IO.
bool lcd_open_amoled(void) {
#if !CONFIG_IDF_TARGET_ESP32S3
    return false;
#else
    if (s_io) return true;
    s_io_done = xSemaphoreCreateBinary();
    if (!s_io_done) return false;
    gpio_config_t rst = { .pin_bit_mask = 1ULL << AM_PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(AM_PIN_RST, 0);   // esp_lcd_sh8601 panel_sh8601_reset: 10 ms low, 150 ms to come up
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(AM_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    spi_bus_config_t bc = { .sclk_io_num = AM_PIN_SCLK, .data0_io_num = AM_PIN_D0, .data1_io_num = AM_PIN_D1,
                            .data2_io_num = AM_PIN_D2, .data3_io_num = AM_PIN_D3, .max_transfer_sz = LCD_W * SH * 2 };
    esp_err_t e = spi_bus_initialize(AM_HOST, &bc, SPI_DMA_CH_AUTO);
    if (e == ESP_OK) {
        esp_lcd_panel_io_spi_config_t ic = { .cs_gpio_num = AM_PIN_CS, .dc_gpio_num = -1, .spi_mode = 0,
                                             .pclk_hz = AM_CLK_HZ, .trans_queue_depth = 2, .on_color_trans_done = am_done,
                                             .lcd_cmd_bits = 32, .lcd_param_bits = 8, .flags = { .quad_mode = 1 } };
        e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)AM_HOST, &ic, &s_io);
        if (e != ESP_OK) spi_bus_free(AM_HOST);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "amoled: QSPI bus failed (%s)", esp_err_to_name(e));
        vSemaphoreDelete(s_io_done);
        s_io_done = NULL;
        s_io = NULL;
        return false;
    }
    s_panel = &AMOLED;
    return true;
#endif
}

// ── AP sightings ──
void lcd_note_ap(const uint8_t *bssid, uint8_t channel) {
    (void)bssid;
    if (!s_task) return;
    int a = lcd_act_index(channel);
    if (a >= 0) s_act[a] = s_act[a] > 226 ? 250 : s_act[a] + 24;
}

// ── panel ──
static void window(int y0, int y1) {
    int x0 = s_panel->xofs, x1 = s_panel->xofs + s_panel->w - 1;
    y0 += s_panel->yofs; y1 += s_panel->yofs;
    const uint8_t ca[] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t ra[] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    cmd(0x2A, ca, 4);
    cmd(0x2B, ra, 4);
    if (!s_panel->qspi) cmd(0x2C, NULL, 0);   // QSPI: the pixel write carries RAMWR itself
}

// `lock`: take the bus for each changed strip only, so the microSD card (an offline run, or the app reading runs off it)
// waits one strip at most, never a whole frame's drawing. The last strip may be partial (the watch: 502 = 31 x 16 + 6).
// Returns how many strips went to the panel. Adds its own time to s_touch_us / s_paint_us / s_io_us (the bench log).
static int64_t s_touch_us, s_paint_us, s_io_us;
static int render(const lcd_frame_t *f, bool lock) {
    int w = s_panel->w, sent = 0;
    for (int k = 0; k * SH < s_panel->h; k++) {
        int y0 = k * SH, rows = s_panel->h - y0 < SH ? s_panel->h - y0 : SH;
        int64_t t0 = esp_timer_get_time();
        touch_tick(now_ms());   // the finger keeps being followed while a long frame draws
        int64_t t1 = esp_timer_get_time();
        lcd_draw_strip(s_strip, y0, f);
        uint32_t h = 2166136261u;
        const uint32_t *px = (const uint32_t *)s_strip;
        for (int i = 0; i < w * rows / 2; i++) h = (h ^ px[i]) * 16777619u;
        int64_t t2 = esp_timer_get_time();
        s_touch_us += t1 - t0;
        s_paint_us += t2 - t1;
        if (h == s_hash[k]) continue;
        if (lock) lcd_bus_lock();
        window(y0, y0 + rows - 1);
        if (send(true, s_strip, (size_t)w * rows * 2) == ESP_OK) { s_hash[k] = h; sent++; }
        if (lock) lcd_bus_unlock();
        s_io_us += esp_timer_get_time() - t2;
    }
    return sent;
}

// One frame's worth of state, gathered once.
static void gather(lcd_frame_t *f, uint32_t now) {
    memset(f, 0, sizeof(*f));
    f->w = s_panel->w;
    f->h = s_panel->h;
    f->st.batt_pct = -1;
    if (s_fill) s_fill(&f->st);
    f->now = now;
    f->t0 = s_t0;
    f->live = f->st.aps_live;
    f->session = f->st.aps_session;
    f->run = f->st.aps_run;
    uint32_t then = s_session_at[(now / 1000 + 1) % 10];   // the oldest of the ten samples
    f->fresh = f->session > then ? f->session - then : 0;
    f->cur = lcd_act_index(f->st.channel);
    for (int i = 0; i < LCD_ACT_N; i++) f->act[i] = s_act[i];
    f->alerting = !f->st.ota && (int32_t)(s_alert_until - now) > 0;   // an update's screen outranks alerts
    f->alert_on = !(((now - s_alert_since) / ALERT_BLINK_MS) & 1);
    memcpy(f->alert, s_alert_label, sizeof(f->alert));
    f->alert_rgb = s_alert_rgb;
    f->alert_police = s_alert_police;
    f->board = s_board;
    f->version = s_version;
    f->bright_pct = s_bright_pct;
    f->sleep_s = s_sleep_s;
#if CONFIG_IDF_TARGET_ESP32S3
    f->page = s_page;
    f->net_first = s_net_first;
    f->hold_btn = s_hold_btn;
    f->hold_pct = s_hold_pct;
#else
    f->hold_btn = LCD_BTN_NONE;
#endif
}

// Backlight PWM, or on an AMOLED (no backlight pin) the panel's own brightness, 0-255 either way.
static void backlight(int duty) {
    if (s_panel->bl < 0) {
        const uint8_t v = (uint8_t)duty;
        lcd_bus_lock();
        cmd(0x51, &v, 1);
        lcd_bus_unlock();
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

#if CONFIG_IDF_TARGET_ESP32S3
// ── the watch's touch pages ──
static uint16_t next_step(const uint16_t *steps, size_t n, uint16_t v) {
    for (size_t i = 0; i < n; i++) if (steps[i] == v) return steps[(i + 1) % n];
    return steps[0];
}

static bool is_step(const uint16_t *steps, size_t n, uint16_t v) {
    for (size_t i = 0; i < n; i++) if (steps[i] == v) return true;
    return false;
}

static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t b;
    uint16_t s;
    if (nvs_get_u8(h, NVS_BRIGHT, &b) == ESP_OK && is_step(BRIGHT_STEPS, sizeof(BRIGHT_STEPS) / sizeof(*BRIGHT_STEPS), b))
        s_bright_pct = b;
    if (nvs_get_u16(h, NVS_SLEEP, &s) == ESP_OK && is_step(SLEEP_STEPS, sizeof(SLEEP_STEPS) / sizeof(*SLEEP_STEPS), s))
        s_sleep_s = s;
    nvs_close(h);
}

static void settings_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGW(TAG, "watch: settings not saved"); return; }
    nvs_set_u8(h, NVS_BRIGHT, s_bright_pct);
    nvs_set_u16(h, NVS_SLEEP, s_sleep_s);
    nvs_commit(h);
    nvs_close(h);
}

/// A button pressed (held): what it does. SCAN acts while a scan runs or a host listens; MODE only while a host listens
/// (the mode resets to Wi-Fi on every link, so one picked with nobody listening would be thrown away).
static void watch_press(int btn) {
    if (btn == LCD_BTN_SCAN) {
        if (!s_scan || !(s_frame.st.scanning || s_frame.st.can_scan)) return;
        ESP_LOGI(TAG, "watch: scan %s", s_frame.st.scanning ? "paused" : "resumed");
        s_scan(LCD_SCAN_PAUSE_RESUME);
    } else if (btn == LCD_BTN_MODE) {
        if (!s_scan || !s_frame.st.can_scan) return;
        ESP_LOGI(TAG, "watch: mode after %s", s_frame.st.mode);
        s_scan(LCD_SCAN_NEXT_MODE);
    } else if (btn == LCD_BTN_BRIGHT) {
        s_bright_pct = (uint8_t)next_step(BRIGHT_STEPS, sizeof(BRIGHT_STEPS) / sizeof(*BRIGHT_STEPS), s_bright_pct);
        settings_save();
        ESP_LOGI(TAG, "watch: brightness %u%%", s_bright_pct);
    } else if (btn == LCD_BTN_SLEEP) {
        s_sleep_s = next_step(SLEEP_STEPS, sizeof(SLEEP_STEPS) / sizeof(*SLEEP_STEPS), s_sleep_s);
        settings_save();
        ESP_LOGI(TAG, "watch: screen off after %u s", s_sleep_s);
    }
}

/// One touch poll through the gestures: pages, scrolling, the held button. True when the picture changes.
static bool watch_input(bool down, int x, int y, uint32_t now) {
    if (s_wake_finger) {   // the touch that woke the panel is only a wake, never a press on what it shows
        if (!down) s_wake_finger = false;
        s_gest = (gest_t){0};
        return false;
    }
    int8_t was_btn = s_hold_btn;
    uint8_t was_pct = s_hold_pct, was_page = s_page, was_first = s_net_first;
    gesture_t g = gest_feed(&s_gest, down, x, y, now);
    uint8_t nets = s_frame.st.nets_n;
    if (g == GEST_LEFT && s_page + 1 < LCD_PAGES) s_page++;
    else if (g == GEST_RIGHT && s_page > 0) s_page--;
    else if (g == GEST_UP && s_page == LCD_PAGE_NETS && s_net_first + LCD_NETS_ROWS < nets) s_net_first += LCD_NETS_ROWS;
    else if (g == GEST_DOWN && s_page == LCD_PAGE_NETS) s_net_first = s_net_first > LCD_NETS_ROWS ? s_net_first - LCD_NETS_ROWS : 0;
    else if (g == GEST_HOLD && s_page == LCD_PAGE_CTRL) watch_press(lcd_watch_button_at(s_gest.x0, s_gest.y0));
    if (g == GEST_LEFT || g == GEST_RIGHT) ESP_LOGI(TAG, "watch: page %u", s_page);
    bool still = s_page == LCD_PAGE_CTRL && s_gest.down && !s_gest.moved && !s_gest.held;
    s_hold_btn = (int8_t)(still ? lcd_watch_button_at(s_gest.x0, s_gest.y0) : LCD_BTN_NONE);
    s_hold_pct = s_hold_btn != LCD_BTN_NONE ? (uint8_t)gest_hold_pct(&s_gest, now) : 0;
    return g == GEST_HOLD || s_page != was_page || s_net_first != was_first || s_hold_btn != was_btn ||
           s_hold_pct != was_pct;
}

// The board's BOOT button (GPIO 0, pulled up on the board, pressed = low: Waveshare 04_Immersive_block main.c:385 and
// Gravitysphere.cpp:397 read it that way). A press moves the board to its next scan mode, like the MODE button, and
// wakes the panel so the new mode shows.
#define BOOT_BTN_GPIO 0
#define BOOT_BTN_DEBOUNCE_MS 30
static void boot_button_init(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOOT_BTN_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
}

static void boot_button_tick(uint32_t now) {
    static bool released;      // seen high since the last press (a button held down at boot never fires)
    static bool low;           // reading low now
    static uint32_t low_at;    // since when
    if (gpio_get_level(BOOT_BTN_GPIO)) { released = true; low = false; return; }
    if (!low) { low = true; low_at = now; return; }
    if (!released || now - low_at < BOOT_BTN_DEBOUNCE_MS) return;
    released = false;
    s_touch_at = now;          // counts as a touch: wakes the panel, restarts the screen-off timer
    if (!s_scan || !s_frame.st.can_scan) { ESP_LOGI(TAG, "watch: BOOT pressed, no host listening"); return; }
    ESP_LOGI(TAG, "watch: BOOT, mode after %s", s_frame.st.mode);
    s_scan(LCD_SCAN_NEXT_MODE);
    s_redraw = true;
}
#endif

// The finger, polled at most every TOUCH_POLL_MS: from the task loop and between strips while a frame draws, so a swipe
// or a hold is seen however long the frame takes. While the panel is dark it only notes the finger (that is the wake).
static void touch_tick(uint32_t now) {
    if (!s_touch || now - s_poll_at < TOUCH_POLL_MS) return;
    s_poll_at = now;
    s_down = s_touch(&s_tx, &s_ty);
    if (s_down) s_touch_at = now;
#if CONFIG_IDF_TARGET_ESP32S3
    boot_button_tick(now);
    if (!s_dark && watch_input(s_down, s_tx, s_ty, now)) s_redraw = true;
#endif
}

static void lcd_task(void *arg) {
    (void)arg;
    bool dark = false;
    int bl = -1;
    uint32_t last_decay = now_ms(), last_active = now_ms(), last_render = 0;
    uint32_t diag_at = now_ms(), frames = 0, strips = 0;
    int64_t draw_us = 0, worst_us = 0, gather_us = 0;
    lcd_frame_t *f = &s_frame;
    for (;;) {
        uint32_t now = now_ms();
        touch_tick(now);
        bool alert = (int32_t)(s_alert_until - now) > 0;
        if (alert) last_active = now;
        if (s_touch && (int32_t)(s_touch_at - last_active) > 0) last_active = s_touch_at;
        bool asleep = s_touch && now - last_active > (uint32_t)s_sleep_s * 1000;
        bool want_dark = s_stealth || asleep;
        if (want_dark != dark) {
            dark = s_dark = want_dark;
            lcd_bus_lock();
            cmd(dark ? 0x28 : 0x29, NULL, 0);
            lcd_bus_unlock();
            if (dark) { backlight(0); bl = 0; }
            else { memset(s_hash, 0, sizeof(s_hash)); last_render = 0; }   // redraw everything on the way back
#if CONFIG_IDF_TARGET_ESP32S3
            if (!dark) s_wake_finger = s_down;
#endif
            if (s_touch && !s_stealth) {
                if (dark) ESP_LOGI(TAG, "lcd: screen off (no touch for %u s)", s_sleep_s);
                else if (s_down) ESP_LOGI(TAG, "lcd: screen on (touch at %d,%d)", s_tx, s_ty);
                else ESP_LOGI(TAG, "lcd: screen on (alert)");
            }
        }
        if (dark) { vTaskDelay(pdMS_TO_TICKS(s_touch ? DARK_POLL_MS : FRAME_MS)); continue; }   // the finger and BOOT still polled

        bool dirty = s_redraw, holding = false;
        s_redraw = false;
#if CONFIG_IDF_TARGET_ESP32S3
        holding = s_hold_btn != LCD_BTN_NONE;
#endif
        // Without touch the loop runs once a frame and draws every time, as before. The watch draws its data once
        // ANIM_STEP_MS (a channel hopping six times a second is flicker, not information), right away on a touch or a new
        // alert, and at the frame's pace only while something moves on purpose: a held button, an alert, the splash,
        // an update.
        bool moving = holding || f->alerting || f->st.ota || now - s_t0 < SPLASH_FAST_MS;
        uint32_t period = holding ? HOLD_FRAME_MS : f->alerting ? ALERT_BLINK_MS / 2 : s_touch && !moving ? ANIM_STEP_MS : FRAME_MS;
        if (!s_touch || dirty || now - last_render + TOUCH_POLL_MS / 2 >= period) {
            int64_t t = esp_timer_get_time();
            gather(f, now);
            gather_us += esp_timer_get_time() - t;
            if (now - last_decay >= 1000) {   // activity fades; a new second's slot starts at zero
                last_decay = now;
                for (int i = 0; i < LCD_ACT_N; i++) s_act[i] = (uint8_t)(s_act[i] * 3 / 4);
                s_session_at[(now / 1000) % 10] = f->session;
            }
            strips += (uint32_t)render(f, true);
            int64_t us = esp_timer_get_time() - t;
            draw_us += us;
            if (us > worst_us) worst_us = us;
            frames++;
            last_render = now;
            int want = BL_LEVEL(s_bright_pct);
            if (want != bl) { backlight(want); bl = want; }
        }
        if (s_touch && now - diag_at >= DIAG_MS && frames) {   // the watch: what drawing costs, for the bench log
            s_diag_frames = (uint16_t)frames;
            s_diag_avg_ms = (uint16_t)(draw_us / frames / 1000);
            s_diag_worst_ms = (uint16_t)(worst_us / 1000);
            if (s_diag_log)
                ESP_LOGI(TAG, "lcd: %lu frames in %lu s, avg %lu ms (paint %lu, panel %lu, touch %lu, state %lu), worst %lu ms, "
                         "%lu strips sent, stack left %lu",
                         (unsigned long)frames, (unsigned long)((now - diag_at) / 1000), (unsigned long)(draw_us / frames / 1000),
                         (unsigned long)(s_paint_us / frames / 1000), (unsigned long)(s_io_us / frames / 1000),
                         (unsigned long)(s_touch_us / frames / 1000), (unsigned long)(gather_us / frames / 1000),
                         (unsigned long)(worst_us / 1000), (unsigned long)strips, (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            diag_at = now; frames = strips = 0; draw_us = worst_us = gather_us = s_paint_us = s_io_us = s_touch_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(s_touch ? TOUCH_POLL_MS : period));
    }
}

void lcd_start(const char *board, const char *version, lcd_fill_fn fill_fn) {
    if (!s_panel || s_task || (!s_panel->qspi && !s_dev)) return;
    s_strip = heap_caps_malloc((size_t)s_panel->w * SH * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip) {
        ESP_LOGW(TAG, "lcd: no memory");
        return;
    }
    s_board = board; s_version = version; s_fill = fill_fn;
    if (!s_bus_lock) s_bus_lock = xSemaphoreCreateMutex();

    if (s_panel->bl >= 0) {
        ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                                   .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK };
        ledc_channel_config_t cc = { .gpio_num = s_panel->bl, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
                                     .timer_sel = LEDC_TIMER_1, .duty = 0, .flags.output_invert = s_panel->bl_low };
        if (ledc_timer_config(&tc) != ESP_OK || ledc_channel_config(&cc) != ESP_OK) ESP_LOGW(TAG, "lcd: backlight PWM failed");
    }

    lcd_bus_lock();
    for (size_t i = 0; i < s_panel->n_init; i++) {
        const init_t *in = &s_panel->init[i];
        if (cmd(in->c, in->p, in->n) != ESP_OK) { lcd_bus_unlock(); ESP_LOGW(TAG, "lcd didn't take its setup"); return; }
        if (in->ms) vTaskDelay(pdMS_TO_TICKS(in->ms));
    }
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_touch) { settings_load(); boot_button_init(); }
#endif
    s_t0 = now_ms();
    gather(&s_frame, s_t0);
    render(&s_frame, false);   // the splash is on the panel before it lights up (the bus is already held)
    cmd(0x29, NULL, 0);
    lcd_bus_unlock();
    backlight(BL_LEVEL(s_bright_pct));
    if (xTaskCreate(lcd_task, "wd_lcd", 4096, NULL, 1, &s_task) != pdPASS) { s_task = NULL; return; }
    ESP_LOGI(TAG, "lcd: %s running", s_panel->name);
}

bool lcd_running(void) { return s_task != NULL; }
long lcd_stack_free(void) { return s_task ? (long)uxTaskGetStackHighWaterMark(s_task) : -1; }
bool lcd_dark(void) { return s_dark; }
void lcd_diag_last(uint16_t *frames, uint16_t *avg_ms, uint16_t *worst_ms) {
    *frames = s_diag_frames; *avg_ms = s_diag_avg_ms; *worst_ms = s_diag_worst_ms;
}
void lcd_diag_log(bool on) { s_diag_log = on; }

// ── microSD slot (the Koko V8 / AWOK Dual C5 layout: card on CS 10 of the panel's bus) ──
bool lcd_sd_slot(int *host, int *cs) {
    if (!s_task || (s_panel != &KOKO && s_panel != &MINI) || !s_sd_cs) return false;   // Koko V8 / AWOK, Marauder Mini v3
    if (host) *host = HOST;
    if (cs) *cs = PIN_SD_CS;
    return true;
}

void lcd_bus_lock(void) { if (s_bus_lock) xSemaphoreTake(s_bus_lock, portMAX_DELAY); }
void lcd_bus_unlock(void) { if (s_bus_lock) xSemaphoreGive(s_bus_lock); }
const char *lcd_size(void) {
    return !s_task ? "" : s_panel->qspi ? "410x502" : s_panel == &MINI ? "128x128" : "240x320";
}

void lcd_alert(const char *label, uint32_t ms, bool soft, uint32_t rgb, bool police) {
    if (!s_task) return;
    uint32_t now = now_ms();
    if (!ms) { s_alert_until = now; return; }   // `led off`: back to the dashboard
    if (soft && (int32_t)(s_alert_until - now) > 0) return;
    size_t n = 0;
    for (; label[n] && n < sizeof(s_alert_label) - 1; n++)   // one word from the app: '_' stands for a space
        s_alert_label[n] = label[n] == '_' ? ' ' : (char)toupper((unsigned char)label[n]);
    s_alert_label[n] = '\0';
    if (!n) strcpy(s_alert_label, "ALERT");
    s_alert_rgb = rgb;
    s_alert_police = police;
    s_alert_since = now;
    s_alert_until = now + ms;
    s_redraw = true;
}

void lcd_set_stealth(bool on) { s_stealth = on; }
void lcd_set_touch(lcd_touch_fn fn) { s_touch = fn; }
void lcd_set_scan(lcd_scan_fn fn) {
#if CONFIG_IDF_TARGET_ESP32S3
    s_scan = fn;
#else
    (void)fn;
#endif
}
