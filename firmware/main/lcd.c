// WardriveESP — color status screens: the Koko Marauder V8's 240x320 ILI9341 and the Marauder Mini v3's 128x128 ST7735
// over SPI, and the Waveshare ESP32-S3 Touch AMOLED 2.06's 410x502 CO5300 over QSPI. Boot splash, live dashboard,
// alerts (scenes in lcd_draw.c). Drawn in 16-row strips into one small DMA buffer; a strip goes to the panel only when
// its pixels changed.
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
#define IDLE_DIM_MS  30000  // nobody linked this long: dim the backlight
#define BL_ON   255
#define BL_DIM  40

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
#define AM_CLEAR_ROWS 8                    // rows per blanking write: 410 x 8 x 2 bytes fits the 240 x 16 strip buffer
// QSPI framing (esp_lcd_sh8601.c): a register write is opcode 02h then 00 <cmd> 00 on one line; pixels are opcode 32h
// then 00 2Ch 00, the data on four lines.
#define AM_OP_CMD     0x02u
#define AM_OP_PIXELS  0x32u
// Every window the CO5300 takes starts on an even column and row and spans an even count (the BSP's LVGL rounder).
// The 240x320 dashboard sits centered, one pixel left and up so both offsets stay even.
#define AM_SCENE_X    (AM_XGAP + ((AM_W - 240) / 2 & ~1))
#define AM_SCENE_Y    ((AM_H - 320) / 2 & ~1)

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
static const panel_t AMOLED = { "410x502 CO5300", 240, 320, AM_SCENE_X, AM_SCENE_Y, -1, false, INIT_CO5300,
                                sizeof(INIT_CO5300) / sizeof(INIT_CO5300[0]), true };
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_io_done;   // the pixels of the last strip are out: the strip buffer can be drawn again
#endif

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
// waits one strip at most, never a whole frame's drawing.
static void render(const lcd_frame_t *f, bool lock) {
    int w = s_panel->w;
    for (int k = 0; k < s_panel->h / SH; k++) {
        int y0 = k * SH;
        lcd_draw_strip(s_strip, y0, f);
        uint32_t h = 2166136261u;
        const uint32_t *px = (const uint32_t *)s_strip;
        for (int i = 0; i < w * SH / 2; i++) h = (h ^ px[i]) * 16777619u;
        if (h == s_hash[k]) continue;
        if (lock) lcd_bus_lock();
        window(y0, y0 + SH - 1);
        if (send(true, s_strip, (size_t)w * SH * 2) == ESP_OK) s_hash[k] = h;
        if (lock) lcd_bus_unlock();
    }
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
// The reset leaves noise in the AMOLED's memory and the dashboard covers only the middle: black the whole panel once.
static void am_clear(void) {
    memset(s_strip, 0, (size_t)AM_W * AM_CLEAR_ROWS * 2);
    const uint8_t ca[] = { 0x00, AM_XGAP, (AM_XGAP + AM_W - 1) >> 8, (AM_XGAP + AM_W - 1) & 0xFF };
    cmd(0x2A, ca, 4);
    for (int y = 0; y < AM_H; y += AM_CLEAR_ROWS) {
        int rows = AM_H - y < AM_CLEAR_ROWS ? AM_H - y : AM_CLEAR_ROWS;
        const uint8_t ra[] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + rows - 1) >> 8), (uint8_t)(y + rows - 1) };
        cmd(0x2B, ra, 4);
        send(true, s_strip, (size_t)AM_W * rows * 2);
    }
}
#endif

static void lcd_task(void *arg) {
    (void)arg;
    bool dark = false;
    int bl = -1;
    uint32_t last_decay = now_ms(), last_linked = now_ms();
    lcd_frame_t f;
    for (;;) {
        uint32_t now = now_ms();
        if (s_stealth != dark) {
            dark = s_stealth;
            lcd_bus_lock();
            cmd(dark ? 0x28 : 0x29, NULL, 0);
            lcd_bus_unlock();
            if (dark) { backlight(0); bl = 0; }
            else memset(s_hash, 0, sizeof(s_hash));   // redraw everything on the way back
        }
        if (dark) { vTaskDelay(pdMS_TO_TICKS(FRAME_MS)); continue; }

        gather(&f, now);
        if (f.st.ble || f.st.usb) last_linked = now;
        if (now - last_decay >= 1000) {   // activity fades; a new second's slot starts at zero
            last_decay = now;
            for (int i = 0; i < LCD_ACT_N; i++) s_act[i] = (uint8_t)(s_act[i] * 3 / 4);
            s_session_at[(now / 1000) % 10] = f.session;
        }
        bool alerting = f.alerting;
        render(&f, true);

        int want = alerting || f.st.ota || now - last_linked <= IDLE_DIM_MS ? BL_ON : BL_DIM;
        if (want != bl) { backlight(want); bl = want; }
        vTaskDelay(pdMS_TO_TICKS(alerting ? ALERT_BLINK_MS / 2 : FRAME_MS));
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
    if (s_panel->qspi) am_clear();
#endif
    s_t0 = now_ms();
    lcd_frame_t f;
    gather(&f, s_t0);
    render(&f, false);   // the splash is on the panel before it lights up (the bus is already held)
    cmd(0x29, NULL, 0);
    lcd_bus_unlock();
    backlight(BL_ON);
    if (xTaskCreate(lcd_task, "wd_lcd", 4096, NULL, 1, &s_task) != pdPASS) { s_task = NULL; return; }
    ESP_LOGI(TAG, "lcd: %s running", s_panel->name);
}

bool lcd_running(void) { return s_task != NULL; }

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
}

void lcd_set_stealth(bool on) { s_stealth = on; }
