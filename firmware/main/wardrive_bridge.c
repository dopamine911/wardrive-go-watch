// WardriveESP — Wi-Fi and Bluetooth scanner for Wardrive Go over BLE and USB. Protocol: PROTOCOL.md
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_system.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_private/panic_internal.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

#include "nimble/nimble_port.h"
#include "esp_bt.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "soc/uart_pins.h"
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
#include "driver/i2c_master.h"
#include "driver/rmt_tx.h"
#include "esp_random.h"
#endif
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/i2c_master.h"
#endif
#include "esp_rom_sys.h"
#include "display.h"
#include "lcd.h"
#include "apcount.h"
#include "sdrun.h"
#if SOC_MODEM_CLOCK_SUPPORTED
#include "esp_private/esp_modem_clock.h"
#include "hal/modem_syscon_ll.h"
#endif
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"   // os_msys_num_free: buffers kept for GATT answers (tx_room)
#include "host/ble_att.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"


static const char *TAG = "WardriveESP";

#define FW_VERSION     "1.7.5"
// Shown by `info` and the boot log only. Never part of FW_VERSION: the apps compare versions number by number, so
// "1.6.6-dev" would read as 1.6.0 and get offered the published firmware as an "update".
#define FW_TAG         ""
#define NAME_PREFIX    "WardriveESP"

// Wi-Fi radar (CSI, PROTOCOL.md "Wi-Fi radar (CSI)", experimental): ESP32-C5 and ESP32-C6 only for now. Every other
// chip reports csi=0 in WD:CAPS and refuses `csi`.
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
#define CSI_SUPPORTED  1
#else
#define CSI_SUPPORTED  0
#endif

// USB transports (PROTOCOL.md "USB"): USB-Serial-JTAG on chips with native USB, UART0 for USB-UART bridge boards.
#define USB_JTAG_LINK  SOC_USB_SERIAL_JTAG_SUPPORTED
#if CONFIG_ESP_CONSOLE_UART && CONFIG_ESP_CONSOLE_UART_NUM == 0 && \
    !(defined(CONFIG_WARDRIVEESP_GPS_UART_NUM) && CONFIG_WARDRIVEESP_GPS_UART_NUM == 0)
#define UART_LINK      1
#define LINK_UART      UART_NUM_0
#else
#define UART_LINK      0
#endif

enum {
    FR_MAGIC_0 = 0x47, FR_MAGIC_1 = 0x42, FR_VERSION = 0x01, FR_HEADER = 12,
    FR_CMD = 1, FR_ACK = 2, FR_DATA = 3, FR_END = 4, FR_ERR = 5,
};

// What the board was doing, kept current in the hot paths and saved by the panic handler so the next boot can
// report it in WD:CRASH.
enum { CR_IDLE = 0, CR_SCAN, CR_RESULTS, CR_SEND_AP, CR_BT, CR_CAP, CR_OTA, CR_CMD, CR_GPS, CR_CSI };
static volatile uint8_t  s_crumb = CR_IDLE;
static volatile uint8_t  s_crumb_ch = 0;
static volatile uint32_t s_heap_min_seen = 0;

#define CRASH_MAGIC 0x10E5C0DEu
typedef struct {
    uint32_t magic;
    uint32_t up_ms;
    uint32_t pc;
    uint32_t heap_min;
    uint8_t  crumb, ch, kind, core;
    char     task[16];
    char     reason[56];
} crash_rec_t;
enum { CK_PANIC = 1, CK_STACK = 2 };
static RTC_NOINIT_ATTR crash_rec_t s_crash;

static void IRAM_ATTR crash_copy(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (src) for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void IRAM_ATTR crash_fill(uint8_t kind, const char *task, const char *reason, uint32_t pc, int core) {
    if (s_crash.magic == CRASH_MAGIC && s_crash.kind == CK_STACK) return;   // keep the more specific record
    s_crash.up_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_crash.pc = pc;
    s_crash.heap_min = s_heap_min_seen;
    s_crash.crumb = s_crumb;
    s_crash.ch = s_crumb_ch;
    s_crash.kind = kind;
    s_crash.core = (uint8_t)core;
    crash_copy(s_crash.task, sizeof(s_crash.task), task);
    crash_copy(s_crash.reason, sizeof(s_crash.reason), reason);
    s_crash.magic = CRASH_MAGIC;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    crash_fill(CK_STACK, name, "stack overflow", 0, 0);
    static char msg[48] = "Stack overflow in task ";   // static: this stack is already exhausted
    crash_copy(msg + 23, sizeof(msg) - 23, name ? name : "?");
    esp_system_abort(msg);
}

extern void __real_esp_panic_handler(panic_info_t *info);
void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info) {
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    crash_fill(CK_PANIC, cur ? pcTaskGetName(cur) : "?", info ? info->reason : NULL,
               info ? (uint32_t)(uintptr_t)info->addr : 0, info ? info->core : 0);
    __real_esp_panic_handler(info);
}

// UUIDs are least-significant byte first.
static const ble_uuid128_t SVC_UUID  = BLE_UUID128_INIT(
    0x76,0x53,0x65,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47);
static const ble_uuid128_t RX_UUID   = BLE_UUID128_INIT(
    0x52,0x58,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x01);
static const ble_uuid128_t TX_UUID   = BLE_UUID128_INIT(
    0x54,0x58,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x02);
static const ble_uuid128_t CTRL_UUID = BLE_UUID128_INIT(
    0x43,0x54,0x52,0x4C,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x03);
static const ble_uuid128_t OTA_UUID  = BLE_UUID128_INIT(
    0x44,0x41,0x54,0x4F,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x04);
static const ble_uuid128_t STATUS_UUID = BLE_UUID128_INIT(   // 0547686F-7374-4553-5042-726964675354 (1.6.5+)
    0x54,0x53,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x05);

#define OTA_WINDOW_MAX  (12 * 1024)   // max bytes the phone sends ahead of the last ack
#define OTA_ACK_STEP    (4 * 1024)
#define OTA_BUF_SLACK   (2 * 1024)

// Receive buffer sizes tried in order; a long-running board's heap may not have the largest block free.
static const size_t OTA_BUF_SIZES[] = { 24 * 1024, 16 * 1024, 10 * 1024, 6 * 1024 };

static StreamBufferHandle_t s_ota_buf;
static size_t s_ota_buf_bytes;
static uint32_t s_ota_window;
static uint32_t s_ota_ack_step;
static TaskHandle_t s_ota_task;
static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_ota_part;
static volatile bool s_ota_active = false;
static volatile bool s_ota_end_req = false;
static volatile bool s_ota_abort_req = false;
static volatile uint32_t s_ota_size = 0;
static volatile uint32_t s_ota_written = 0;   // image bytes written so far (the screens' progress)

// Output sinks: the BLE phone and one session per USB port. Every line goes to every linked host.
enum { SINK_BLE = 1, SINK_USB = 2, SINK_UART = 4, SINK_ALL = 7 };
static volatile uint8_t s_usb_sessions = 0;      // SINK_USB / SINK_UART ports with an open session
static volatile uint8_t s_ota_src = SINK_BLE;    // host running the update; gets the WD:OTA lines
static volatile uint8_t s_cmd_src = SINK_BLE;    // host whose command is running (under s_cmd_lock)

// Every channel the radio can tune. 5 GHz is always a passive scan / promiscuous RX here.
static const uint8_t CH_ORDER[] = { 1, 6, 11, 36, 40, 44, 48, 149, 153, 157, 161,
                                    52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
                                    165, 169, 173, 177,
                                    2, 3, 4, 5, 7, 8, 9, 10, 12, 13, 14 };
#define CH_ORDER_N       (sizeof(CH_ORDER) / sizeof(CH_ORDER[0]))
#define MAX_CHANNELS     CH_ORDER_N
#define PASSIVE_DWELL_MS 120
#define MAX_AP_PER_SCAN  60
#define CMD_MAX          240

static uint8_t  s_supported[MAX_CHANNELS];
static size_t   s_supported_n = 0;
static bool     s_has_5g = false;

static uint8_t  s_plan[MAX_CHANNELS];
static size_t   s_plan_n = 0;
static portMUX_TYPE s_plan_lock = portMUX_INITIALIZER_UNLOCKED;

static char     s_name[24] = NAME_PREFIX;
static uint8_t  s_own_addr_type;
static uint8_t  s_addr_tail[2];   // the two address bytes in the name ("WardriveESP-%02X%02X")
// Status beacon state (1.6.5, see start_beacon).
#define BEACON_FIXED 9
#define HOLDER_MAX   18
static char s_holder[HOLDER_MAX + 1];      // the linked app's nickname ("iam a|i <nickname>"), "" = didn't say
static bool s_holder_ios = false;
// How the last link ended, for the advert extras (1.7.3, see ext_build).
enum { LINK_END_NONE = 0, LINK_END_RELEASED = 1, LINK_END_LOST = 2 };
static uint8_t s_link_end = LINK_END_NONE;
static bool s_release_req = false;   // this Bluetooth link's app said `bye`: it's letting the board go on purpose
static struct ble_npl_callout s_beacon_tick;
static struct ble_npl_callout s_link_switch;   // USB session opened/closed: hand the Bluetooth side over (usb_link_switch)
static struct ble_npl_callout s_cmd_drain;     // runs commands written over BLE after their write response (cmd_drain)
static bool s_link_ready = false;              // the callouts exist (on_ble_sync)
static TaskHandle_t s_host_task = NULL;        // the NimBLE host task (set in on_ble_sync)
static volatile uint32_t s_tx_dropped = 0;     // BLE output frames dropped for lack of buffers (WD:HEALTH txdrop)
static uint16_t s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle = 0;
static volatile bool s_notify_enabled = false;
static volatile bool s_streaming      = false;
static TaskHandle_t  s_scan_task      = NULL;
static volatile uint32_t s_reply_id   = 1;
static volatile uint32_t s_ap_count   = 0;
static volatile uint32_t s_sweeps     = 0;
static volatile uint32_t s_last_aps   = 0;

static bool host_linked(void) { return s_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_usb_sessions; }
/// The Wi-Fi scan runs while a host holds the board, or with nobody linked while an offline run goes to the card.
static bool scan_wanted(void) { return host_linked() || sdrun_active(); }
static volatile bool s_bt_up = true;        // Bluetooth controller + host running (1.6.6: off while USB holds the board)
static volatile bool s_ble_synced = false;  // the host is synced with the controller, so GAP calls work
#if CONFIG_WARDRIVEESP_BENCH_TOOLS
static bool s_bench_keep_bt;   // `benchbt on`: keep Bluetooth up during USB sessions, to compare
#else
#define s_bench_keep_bt false
#endif

#if CONFIG_WARDRIVEESP_BENCH_TOOLS
// Bench builds: walk every heap at key points and log the first one that finds it corrupt, to pin down who corrupts it.
static volatile const char *s_heap_bad_at;
static void heap_check(const char *where) {
    if (s_heap_bad_at || xPortInIsrContext()) return;
    if (!heap_caps_check_integrity_all(true)) {
        s_heap_bad_at = where;
        esp_rom_printf("HEAPCHK FAIL first at %s (up %lu ms)\n", where, (unsigned long)(esp_timer_get_time() / 1000));
    }
}
#else
#define heap_check(where) ((void)0)
#endif

// Scan role: the board does exactly one of these at a time. A role switch lets any running task finish first,
// then the task that ends starts the new role (see set_role / stream_start). Reset to Wi-Fi when a host connects
// or disconnects with no other host linked. ROLE_CAP (1.5+) is passive 802.11 promiscuous capture — handshakes, PMKIDs, client sightings.
// ROLE_CSI (experimental, CSI_SUPPORTED chips only) is the Wi-Fi radar: CSI from one transmitter on one channel, only
// ever entered by `csi start` / `csi tx`.
typedef enum { ROLE_WIFI = 0, ROLE_BT, ROLE_CAP, ROLE_CSI } scan_role_t;
static volatile scan_role_t s_role = ROLE_WIFI;
static const char *role_name(scan_role_t role);

// Bluetooth role: the board scans BLE instead of Wi-Fi.
#define BT_WINDOW_MS  10000
#define BT_QUEUE_LEN  48

typedef struct {
    uint8_t  mac[6];      // most significant byte first
    uint8_t  addr_type;
    int8_t   rssi;
    uint8_t  conn;
    uint8_t  name_len;
    char     name[29];
    uint8_t  has_company;
    uint16_t company;
} bt_rec_t;

static QueueHandle_t s_bt_queue;
static TaskHandle_t  s_bt_task          = NULL;
static volatile bool s_bt_running       = false;
static volatile bool s_bt_window_done   = false;
static volatile bool s_switch_pending   = false;
static volatile uint32_t s_bt_count     = 0;
static volatile uint32_t s_bt_windows   = 0;
static volatile uint32_t s_bt_last      = 0;
static volatile uint32_t s_bt_dropped   = 0;

// GPS role (PROTOCOL.md "GPS", firmware 1.4+): an optional UART NMEA receiver. The task runs continuously and
// emits WD:GPS whenever it parses a valid fix; the phone uses it as a location source when its own fix is stale
// or absent. `s_gps_present` is set only once a valid fix is parsed (runtime presence-detection, like s_has_5g),
// so a board without a GPS wired up never claims the capability. It's read by WD:CAPS whether or not GPS is
// compiled in (always 0 then), so it lives outside the build guard; the fix fields only exist when GPS is on.
static volatile bool s_gps_present = false;
static TaskHandle_t s_gps_task = NULL;

static int  on_gap_event(struct ble_gap_event *event, void *arg);
static void start_advertising(void);
static void stream_start(void);
static void bt_power_kick(void);

static bool is_5ghz(uint8_t ch) { return ch > 14; }
static bool is_dfs(uint8_t ch) { return (ch >= 52 && ch <= 64) || (ch >= 100 && ch <= 144); }

static bool channel_supported(uint8_t ch) {
    for (size_t i = 0; i < s_supported_n; i++) if (s_supported[i] == ch) return true;
    return false;
}

// Open the radio to every channel it can tune (2.4 GHz 1-14, all 5 GHz) so the scan list isn't trimmed.
static void open_all_channels(void) {
    wifi_country_t all = {
        .cc = "01",
        .schan = 1,
        .nchan = 14,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
#if SOC_WIFI_SUPPORT_5G
    all.wifi_5g_channel_mask = 0xFFFFFFFFu;
#endif
    esp_err_t err = esp_wifi_set_country(&all);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_country: %s", esp_err_to_name(err));
}

static void build_supported_channels(void) {
    s_supported_n = 0;
    for (size_t i = 0; i < CH_ORDER_N; i++) {
        uint8_t ch = CH_ORDER[i];
        if (is_5ghz(ch) && !s_has_5g) continue;
        s_supported[s_supported_n++] = ch;
    }
}

static void plan_reset_to_all(void) {
    portENTER_CRITICAL(&s_plan_lock);
    memcpy(s_plan, s_supported, s_supported_n);
    s_plan_n = s_supported_n;
    portEXIT_CRITICAL(&s_plan_lock);
}

static size_t plan_copy(uint8_t *out) {
    portENTER_CRITICAL(&s_plan_lock);
    size_t n = s_plan_n;
    memcpy(out, s_plan, n);
    portEXIT_CRITICAL(&s_plan_lock);
    return n;
}

static size_t parse_channel_list(const char *s, uint8_t *out) {
    size_t n = 0;
    while (*s && n < MAX_CHANNELS) {
        while (*s == ',' || *s == ' ') s++;
        if (*s < '0' || *s > '9') break;
        long v = strtol(s, (char **)&s, 10);
        if (v <= 0 || v > 255 || !channel_supported((uint8_t)v)) continue;
        bool dup = false;
        for (size_t i = 0; i < n; i++) if (out[i] == (uint8_t)v) dup = true;
        if (!dup) out[n++] = (uint8_t)v;
    }
    return n;
}

static int format_channel_list(char *buf, int len, int cap, const uint8_t *ch, size_t n) {
    for (size_t i = 0; i < n && len < cap - 5; i++)
        len += snprintf(buf + len, cap - len, i ? ",%u" : "%u", (unsigned)ch[i]);
    return len;
}

static size_t max_payload_per_notify(void) {
    uint16_t mtu = (s_bt_up && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(s_conn_handle) : 23;
    if (mtu < 23) mtu = 23;
    size_t room = (size_t)mtu - 3;
    return room > FR_HEADER ? room - FR_HEADER : 1;
}

// Serializes each line to every host, so one line's BLE frames and USB bytes are never interleaved with another's
// (scan, GPS, health and command tasks all send).
static SemaphoreHandle_t s_send_lock;

// Not put_le32: NimBLE's os/endian.h defines that name.
static void frame_put_u32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Buffers shared by every sender; only touched while s_send_lock is held. Kept off the task stacks.
static uint8_t s_frame[FR_HEADER + 256];
static char    s_line[384];   // WD:CAPS with every board key and 42 channels runs past 256

// BLE buffers kept free for the answers to the phone's GATT requests (write responses, service discovery). Field
// 2026-10-26, 16 boards on one iPhone: each link drained slowly, a board's output took every buffer, the answer to the
// phone's request couldn't be built, and the phone dropped the link exactly 30 s later (its GATT timeout).
#define TX_RESERVE 4

/// Room for one more output frame while keeping TX_RESERVE free. Senders other than the BLE host task wait up to 100 ms
/// for the link to drain (backpressure instead of loss); the host task never waits.
static bool tx_room(void) {
    if (os_msys_num_free() >= TX_RESERVE) return true;
    if (xTaskGetCurrentTaskHandle() == s_host_task) return false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (!s_bt_up || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
        if (os_msys_num_free() >= TX_RESERVE) return true;
    }
    return false;
}

static void send_frame(uint8_t type, const uint8_t *payload, size_t len) {
    if (!s_bt_up || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled || s_tx_val_handle == 0) return;
    if (!tx_room()) { s_tx_dropped++; return; }
    uint8_t *frame = s_frame;
    if (len > sizeof(s_frame) - FR_HEADER) len = sizeof(s_frame) - FR_HEADER;
    memset(frame, 0, FR_HEADER);
    frame[0] = FR_MAGIC_0; frame[1] = FR_MAGIC_1; frame[2] = FR_VERSION; frame[3] = type;
    frame_put_u32(frame + 6, s_reply_id);
    frame[10] = len & 0xff; frame[11] = (len >> 8) & 0xff;
    if (len) memcpy(frame + FR_HEADER, payload, len);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)(FR_HEADER + len));
    if (om) (void)ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    else s_tx_dropped++;
}

static void send_ack(void) {
    if (!s_bt_up || s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled || s_tx_val_handle == 0) return;
    if (!tx_room()) { s_tx_dropped++; return; }
    uint8_t frame[FR_HEADER] = { FR_MAGIC_0, FR_MAGIC_1, FR_VERSION, FR_ACK };
    frame_put_u32(frame + 6, s_reply_id);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, FR_HEADER);
    if (om) (void)ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
}

static void send_chunks_locked(const char *buf, size_t n) {
    size_t step = max_payload_per_notify();
    for (size_t off = 0; off < n; off += step) {
        size_t chunk = (n - off < step) ? n - off : step;
        send_frame(FR_DATA, (const uint8_t *)buf + off, chunk);
    }
}

static void usb_emit_locked(uint8_t sinks, const char *buf, size_t n);

// The one output path: a built line (newline included) to the selected hosts. BLE gets MTU-sized DATA frames,
// USB sessions get the raw line.
static void emit_locked(uint8_t sinks, const char *buf, size_t n) {
    if (sinks & SINK_BLE) send_chunks_locked(buf, n);
    if (sinks & s_usb_sessions) usb_emit_locked(sinks, buf, n);
}

// For lines longer than s_line (the capture role's WD:HS).
static void send_chunks(const char *buf, size_t n) {
    if (!s_send_lock) return;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    emit_locked(SINK_ALL, buf, n);
    xSemaphoreGive(s_send_lock);
}

static void send_vline(uint8_t sinks, const char *fmt, va_list ap) {
    if (!s_send_lock) return;
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    int n = vsnprintf(s_line, sizeof(s_line) - 1, fmt, ap);
    if (n > 0) {
        if (n > (int)sizeof(s_line) - 2) n = (int)sizeof(s_line) - 2;
        s_line[n++] = '\n';
        emit_locked(sinks, s_line, (size_t)n);
    }
    xSemaphoreGive(s_send_lock);
}

static void send_line_to(uint8_t sinks, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    send_vline(sinks, fmt, ap);
    va_end(ap);
}

static void send_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    send_vline(SINK_ALL, fmt, ap);
    va_end(ap);
}

struct auth_name { wifi_auth_mode_t mode; const char *token; };
static const struct auth_name AUTH_NAMES[] = {
    { WIFI_AUTH_OPEN,            "OPEN" },
    { WIFI_AUTH_WEP,             "WEP" },
    { WIFI_AUTH_WPA_PSK,         "WPA" },
    { WIFI_AUTH_WPA2_PSK,        "WPA2" },
    { WIFI_AUTH_WPA_WPA2_PSK,    "WPA2" },
    { WIFI_AUTH_WPA2_ENTERPRISE, "WPA2" },
    { WIFI_AUTH_WPA3_PSK,        "WPA3" },
    { WIFI_AUTH_WPA2_WPA3_PSK,   "WPA2_WPA3" },
};

static const char *auth_name(wifi_auth_mode_t mode) {
    for (size_t i = 0; i < sizeof(AUTH_NAMES) / sizeof(AUTH_NAMES[0]); i++)
        if (AUTH_NAMES[i].mode == mode) return AUTH_NAMES[i].token;
    return "UNKNOWN";
}

static char lower_hex_digit(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

static void sd_note_ap(const wifi_ap_record_t *ap, size_t ssid_len);   // the offline run (see "microSD")

static void send_ap_line(const wifi_ap_record_t *ap) {
    size_t ssid_len = 0;
    while (ssid_len < sizeof(ap->ssid) && ap->ssid[ssid_len]) ssid_len++;
    char ssid_hex[2 * sizeof(ap->ssid) + 1];
    for (size_t i = 0; i < ssid_len; i++) {
        ssid_hex[2 * i]     = lower_hex_digit(ap->ssid[i] >> 4);
        ssid_hex[2 * i + 1] = lower_hex_digit(ap->ssid[i] & 0x0F);
    }
    ssid_hex[2 * ssid_len] = '\0';
    const uint8_t *b = ap->bssid;
    send_line("WD:AP ts=%lu bssid=%02X:%02X:%02X:%02X:%02X:%02X ssid_hex=%s rssi=%d ch=%u auth=%s hidden=%u",
              (unsigned long)(esp_timer_get_time() / 1000), b[0], b[1], b[2], b[3], b[4], b[5],
              ssid_hex, ap->rssi, (unsigned)ap->primary, auth_name(ap->authmode), ssid_len ? 0u : 1u);
    s_ap_count++;
    apc_add(ap->bssid, (uint32_t)(esp_timer_get_time() / 1000));
    display_note_ap(ap->bssid, ap->primary);
    lcd_note_ap(ap->bssid, ap->primary);
    if (sdrun_active()) sd_note_ap(ap, ssid_len);
}

static const char *sink_name(uint8_t sink) { return sink == SINK_USB ? "usb" : sink == SINK_UART ? "uart" : "ble"; }

static int  s_led_n;      // WS2812 pixels this board drives, 0 = none (see "Board identity" below)
static char s_board_caps[64];   // "board=<id> board_name=<Name_With_Underscores> " once a board is identified
static bool s_has_batt;   // a battery gauge answered (on this chip's I2C, or through a co-processor)
// Biscuit co-processor radio (1.6.8, see "Biscuit co-processor" below): "coproc=<mode> " for WD:CAPS once a co-processor
// with a second radio said hello, and the hosts that asked for its results (`coproc`), which alone get them.
static char s_cp_caps[24];
static volatile uint8_t s_cp_sinks;
static volatile int s_batt_rate;          // the co-processor gauge's charge rate, hundredths of a percent per hour
static volatile bool s_batt_rate_ok;
static void cmd_coproc(const char *arg);   // the co-processor radio (see "Biscuit co-processor")
static void cmd_sd(const char *arg, uint8_t sink, bool session);   // the microSD card (see "microSD offline runs")
static void clock_set(time_t t, const char *from);
static volatile bool s_clock_set = false;   // the clock holds real UTC (GPS time or the app's `time`)
static void coproc_info(void);

// One WD:CAPS per host, each naming the transport it is sent on. A board with a ring adds led=ring<N>, one with a
// battery gauge batt=1.
static void send_caps_to(uint8_t sinks) {
    static const uint8_t EACH[] = { SINK_BLE, SINK_USB, SINK_UART };
    char buf[384];
    for (size_t i = 0; i < sizeof(EACH); i++) {
        if (!(sinks & EACH[i]) || (EACH[i] != SINK_BLE && !(s_usb_sessions & EACH[i]))) continue;
        char extra[128] = "";
        strlcpy(extra, s_board_caps, sizeof(extra));
        if (s_led_n) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "led=ring%d ", s_led_n);
        if (s_has_batt) strlcat(extra, "batt=1 ", sizeof(extra));
        if (s_cp_caps[0]) strlcat(extra, s_cp_caps, sizeof(extra));
        if (display_running()) strlcat(extra, "disp=128x32 ", sizeof(extra));
        if (lcd_running()) snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "disp=%s ", lcd_size());
        if (sdrun_mounted()) {
            uint32_t total_mb, free_mb;
            sdrun_space(&total_mb, &free_mb);
            snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "sd=1 sd_free_mb=%lu ", (unsigned long)free_mb);
        }
        int len = snprintf(buf, sizeof(buf), "WD:CAPS fw=%s chip=%s bands=%s ota=1 bt=1 gps=%d cap=1 usb=1 csi=%d %slink=%s chans=",
                           FW_VERSION, CONFIG_IDF_TARGET, s_has_5g ? "2g,5g" : "2g", s_gps_present ? 1 : 0,
                           CSI_SUPPORTED ? 1 : 0, extra, sink_name(EACH[i]));
        format_channel_list(buf, len, sizeof(buf), s_supported, s_supported_n);
        send_line_to(EACH[i], "%s", buf);
    }
}

static void send_caps_line(void) { send_caps_to(SINK_ALL); }

static void send_plan_to(uint8_t sinks) {
    uint8_t plan[MAX_CHANNELS];
    size_t n = plan_copy(plan);
    char buf[200];
    int len = snprintf(buf, sizeof(buf), "WD:PLAN chans=");
    format_channel_list(buf, len, sizeof(buf), plan, n);
    send_line_to(sinks, "%s", buf);
}

static void send_plan_line(void) { send_plan_to(SINK_ALL); }

// ─── Bluetooth role (PROTOCOL.md "Bluetooth") ─────────────────────────────
// Discovery runs in BT_WINDOW_MS windows with duplicate filtering, so each device is reported about once per
// window. Reports are queued from the host task and sent from bt_task.
static const char *bt_addr_type_name(uint8_t t) {
    return (t == BLE_ADDR_PUBLIC || t == BLE_ADDR_PUBLIC_ID) ? "public" : "random";
}

static void send_bt_line(const bt_rec_t *r) {
    char name_hex[2 * sizeof(r->name) + 1];
    for (size_t i = 0; i < r->name_len; i++) {
        name_hex[2 * i]     = lower_hex_digit((uint8_t)r->name[i] >> 4);
        name_hex[2 * i + 1] = lower_hex_digit((uint8_t)r->name[i] & 0x0F);
    }
    name_hex[2 * r->name_len] = '\0';
    char mfg[8] = "";
    if (r->has_company) snprintf(mfg, sizeof(mfg), "%04X", (unsigned)r->company);
    const uint8_t *m = r->mac;
    send_line("WD:BT ts=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X type=%s rssi=%d name_hex=%s mfg=%s conn=%u",
              (unsigned long)(esp_timer_get_time() / 1000), m[0], m[1], m[2], m[3], m[4], m[5],
              bt_addr_type_name(r->addr_type), r->rssi, name_hex, mfg, (unsigned)r->conn);
    s_bt_count++;
}

static int on_disc_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    if (event->type == BLE_GAP_EVENT_DISC) {
        const struct ble_gap_disc_desc *d = &event->disc;
        bt_rec_t r = {0};
        for (int i = 0; i < 6; i++) r.mac[i] = d->addr.val[5 - i];
        r.addr_type = d->addr.type;
        r.rssi = d->rssi;
        r.conn = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0) {
            if (f.name && f.name_len) {
                r.name_len = f.name_len > sizeof(r.name) ? sizeof(r.name) : f.name_len;
                memcpy(r.name, f.name, r.name_len);
            }
            if (f.mfg_data && f.mfg_data_len >= 2) {
                r.company = (uint16_t)(f.mfg_data[0] | (f.mfg_data[1] << 8));
                r.has_company = 1;
            }
        }
        if (!s_bt_queue || xQueueSend(s_bt_queue, &r, 0) != pdTRUE) s_bt_dropped++;
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s_bt_window_done = true;
    }
    return 0;
}

static int bt_disc_start(void) {
    struct ble_gap_disc_params p = {
        .itvl = 0x50, .window = 0x30,   // 50 ms interval, 30 ms window: leaves room for the phone link
        .filter_policy = 0, .limited = 0, .passive = 0, .filter_duplicates = 1,
    };
    s_bt_window_done = false;
    return ble_gap_disc(s_own_addr_type, BT_WINDOW_MS, &p, on_disc_event, NULL);
}

static void bt_task(void *arg) {
    (void)arg;
    s_crumb = CR_BT;
    send_line("WD:MODE mode=bt");
    send_line("WD:BEGIN type=bt interval=0 channel=auto");
    s_bt_windows = 0;
    s_bt_count = 0;
    s_bt_dropped = 0;
    for (int t = 0; !s_ble_synced && t < 250 && s_bt_running; t++) vTaskDelay(pdMS_TO_TICKS(20));   // Bluetooth starting
    int fails = 0;
    int rc = bt_disc_start();
    bool scanning = (rc == 0);
    if (!scanning) { send_line("WD:ERR reason=bt_scan_failed rc=%d", rc); fails = 1; }
    bt_rec_t r;
    while (s_bt_running && host_linked()) {
        if (xQueueReceive(s_bt_queue, &r, pdMS_TO_TICKS(100)) == pdTRUE) { send_bt_line(&r); continue; }
        if (scanning && !s_bt_window_done) continue;
        if (scanning) {
            s_bt_windows++;
            s_bt_last = s_bt_count;
            send_line("WD:STATUS bt=%lu scans=%lu dropped=%lu", (unsigned long)s_bt_count,
                      (unsigned long)s_bt_windows, (unsigned long)s_bt_dropped);
            s_bt_count = 0;
            s_bt_dropped = 0;
        }
        rc = bt_disc_start();
        scanning = (rc == 0);
        if (scanning) { fails = 0; continue; }
        send_line("WD:ERR reason=bt_scan_failed rc=%d", rc);
        if (++fails >= 5) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    for (int i = 1; xQueueReceive(s_bt_queue, &r, 0) == pdTRUE; i++) {   // send what was queued before the stop
        send_bt_line(&r);
        if (i % 4 == 0) vTaskDelay(1);
    }
    send_line("WD:END reason=stop");
    s_bt_running = false;
    s_bt_task = NULL;
    bt_power_kick();
    if (s_switch_pending) { s_switch_pending = false; stream_start(); }
    vTaskDelete(NULL);
}

static void bt_start(void) {
    if (s_bt_running || s_bt_task || s_ota_active || !host_linked()) return;
    if (!s_bt_queue) s_bt_queue = xQueueCreate(BT_QUEUE_LEN, sizeof(bt_rec_t));
    if (!s_bt_queue) { send_line("WD:ERR reason=no_memory"); return; }
    s_bt_running = true;
    if (xTaskCreate(bt_task, "wd_bt", 6144, NULL, 5, &s_bt_task) != pdPASS) {
        s_bt_running = false;
        s_bt_task = NULL;
        send_line("WD:ERR reason=no_task");
        return;
    }
    ESP_LOGI(TAG, "bluetooth scan started");
}

// ─── GPS role (PROTOCOL.md "GPS", firmware 1.4+) ──────────────────────────
#if CONFIG_WARDRIVEESP_GPS_ENABLE
static volatile double s_gps_lat = 0, s_gps_lon = 0, s_gps_alt = 0;
static volatile bool   s_gps_has_alt = false;
static volatile int    s_gps_sats = 0;
static volatile bool   s_gps_dirty = false;   // a new valid fix parsed since the last WD:GPS emit
static volatile uint32_t s_gps_fix_ms = 0;    // when the last valid fix was parsed (offline run rows)
static volatile float  s_gps_hdop = 0;

// Fixed-point format a signed double into `buf` with `dec` fractional digits (scale = 10^dec), using only
// integer printf conversions — the firmware may be built with newlib's nano formatting, which drops %f, so
// WD:GPS coordinates must not go through %f. lat/lon/alt all fit comfortably in a 32-bit long after scaling.
static void fmt_fixed(char *buf, size_t cap, double v, long scale, int dec) {
    bool neg = v < 0; if (neg) v = -v;
    long scaled = (long)(v * (double)scale + 0.5);
    snprintf(buf, cap, "%s%ld.%0*ld", neg ? "-" : "", scaled / scale, dec, scaled % scale);
}

// NMEA ddmm.mmmm (lat) / dddmm.mmmm (lon) → signed decimal degrees. Hemisphere 'S'/'W' negates.
static double nmea_coord(const char *v, const char *hemi) {
    if (!v || !*v) return 0;
    double raw = atof(v);
    double deg = (double)((int)(raw / 100.0));
    double val = deg + (raw - deg * 100.0) / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 's' || *hemi == 'W' || *hemi == 'w')) val = -val;
    return val;
}

static void clock_set(time_t t, const char *from);

/// RMC's UTC time (hhmmss[.ss]) and date (ddmmyy) set the board's clock, so offline rows carry real times.
static void gps_set_clock(const char *hms, const char *dmy) {
    if (strlen(hms) < 6 || strlen(dmy) != 6) return;
    int hh = (hms[0] - '0') * 10 + hms[1] - '0', mi = (hms[2] - '0') * 10 + hms[3] - '0', ss = (hms[4] - '0') * 10 + hms[5] - '0';
    int d = (dmy[0] - '0') * 10 + dmy[1] - '0', mo = (dmy[2] - '0') * 10 + dmy[3] - '0', y = 2000 + (dmy[4] - '0') * 10 + dmy[5] - '0';
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || hh > 23 || mi > 59 || ss > 60) return;
    int yy = mo <= 2 ? y - 1 : y, era = yy / 400, yoe = yy - era * 400;   // days from 1970-01-01 (civil calendar)
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    clock_set((time_t)(days * 86400L + hh * 3600L + mi * 60L + ss), "gps");
}

// Parse one NMEA sentence (NUL-terminated, no trailing CR/LF). We read $??GGA (lat/lon/alt/fix/sats) and
// $??RMC (lat/lon/fix) — any talker id (GP/GN/GA/GL/GB). A fix updates the shared state and flags an emit.
static void handle_nmea(char *line) {
    if (line[0] != '$' || strlen(line) < 7) return;
    const char *type = line + 3;                 // skip '$' + 2-char talker id
    bool gga = (strncmp(type, "GGA", 3) == 0);
    bool rmc = (strncmp(type, "RMC", 3) == 0);
    if (!gga && !rmc) return;
    char *star = strchr(line, '*'); if (star) *star = '\0';   // drop the *CS checksum
    // Split on commas, preserving empty fields (strtok would collapse them — NMEA has many).
    char *f[20]; int nf = 0; f[nf++] = line;
    for (char *p = line; *p && nf < 20; p++) if (*p == ',') { *p = '\0'; f[nf++] = p + 1; }

    double lat, lon;
    if (gga) {
        if (nf < 10 || atoi(f[6]) <= 0) return;  // fix quality 0 = no fix
        lat = nmea_coord(f[2], f[3]); lon = nmea_coord(f[4], f[5]);
        if (lat == 0 && lon == 0) return;
        s_gps_lat = lat; s_gps_lon = lon;
        s_gps_sats = atoi(f[7]);
        if (f[8][0]) s_gps_hdop = (float)atof(f[8]);
        if (f[9][0]) { s_gps_alt = atof(f[9]); s_gps_has_alt = true; }
    } else {                                     // RMC — no altitude/sats; keep the last GGA's
        if (nf < 7 || f[2][0] != 'A') return;    // status V = void (no fix)
        lat = nmea_coord(f[3], f[4]); lon = nmea_coord(f[5], f[6]);
        if (lat == 0 && lon == 0) return;
        s_gps_lat = lat; s_gps_lon = lon;
        if (nf >= 10) gps_set_clock(f[1], f[9]);
    }
    s_gps_present = true;                         // runtime presence-detect: now WD:CAPS can advertise gps=1
    s_gps_dirty = true;
    s_gps_fix_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

/// Where the board is for an offline run's row: the last fix, if it is at most 5 s old.
static void gps_where(sdrun_ap_t *r) {
    uint32_t at = s_gps_fix_ms;
    r->fix = at && (uint32_t)(esp_timer_get_time() / 1000) - at < 5000;
    if (!r->fix) return;
    r->lat = s_gps_lat; r->lon = s_gps_lon; r->alt = s_gps_has_alt ? s_gps_alt : 0;
    r->acc_m = s_gps_hdop > 0 ? s_gps_hdop * 5.0f : 0;   // the usual rough figure: HDOP x 5 m
}

static bool board_pin_busy(int gpio);
static bool board_is_awok(void);
static bool board_gps14(void);
// AWOK Dual C5 (its screen chip), Marauder Mini v3 and Koko V8: the GPS module sits on GPIO 14. Marauder's MARAUDER_V8 and
// MARAUDER_MINI_V3 builds open it with Serial2.begin(baud, 8N1, 14, 13) (GPS_TX 14, GPS_RX 13), and Arduino takes the
// RX pin first. Marauder leaves the module at 115200; from power-up it's at 9600. GPIO 13/14 are the C5's native USB,
// so only on these boards (their USB goes through a USB-UART bridge).
#define AWOK_GPS_RX 14

// GPS pins to listen on, first answer wins: the configured pin (read exactly as before), then on an ESP32-C6 GPIO 2 @
// 9600, where some boards (the Rabbit Labs Ghost Board) have a module built in. Input only, so a pin with nothing
// wired to it costs nothing.
static const struct { int rx, baud; } GPS_TRY[] = {
    { CONFIG_WARDRIVEESP_GPS_RX_GPIO, CONFIG_WARDRIVEESP_GPS_BAUD },
#if CONFIG_IDF_TARGET_ESP32C6
    { 2, 9600 },
#endif
};

/// A GPS module streams NMEA ("$G…") from power-up, fix or not.
static bool gps_listen(uart_port_t port, int rx, int baud, uint32_t ms) {
    uart_set_baudrate(port, (uint32_t)baud);
    uart_set_pin(port, UART_PIN_NO_CHANGE, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(port);
    uint8_t buf[128];
    char prev = 0;
    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    while ((uint32_t)(esp_timer_get_time() / 1000) - t0 < ms) {
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) { if (prev == '$' && buf[i] == 'G') return true; prev = (char)buf[i]; }
    }
    return false;
}

static void gps_task(void *arg) {
    (void)arg;
    const uart_port_t port = CONFIG_WARDRIVEESP_GPS_UART_NUM;
    uart_config_t cfg = {
        .baud_rate = CONFIG_WARDRIVEESP_GPS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(port, 1024, 0, 0, NULL, 0) != ESP_OK) { vTaskDelete(NULL); return; }
    uart_param_config(port, &cfg);
    int tx = CONFIG_WARDRIVEESP_GPS_TX_GPIO, rx = -1, baud = CONFIG_WARDRIVEESP_GPS_BAUD;
    const bool awok = board_gps14();
    if (awok) {
        static const int AWOK_BAUD[] = { 115200, 9600 };
        for (size_t i = 0; i < 2 && rx < 0; i++)
            if (gps_listen(port, AWOK_GPS_RX, AWOK_BAUD[i], 2500)) { rx = AWOK_GPS_RX; baud = AWOK_BAUD[i]; }
    }
    for (size_t i = 0; i < sizeof(GPS_TRY) / sizeof(GPS_TRY[0]) && rx < 0 && !awok; i++) {
        if (board_pin_busy(GPS_TRY[i].rx)) continue;
        if (gps_listen(port, GPS_TRY[i].rx, GPS_TRY[i].baud, 2500)) { rx = GPS_TRY[i].rx; baud = GPS_TRY[i].baud; }
        else if (GPS_TRY[i].rx != CONFIG_WARDRIVEESP_GPS_RX_GPIO) gpio_reset_pin(GPS_TRY[i].rx);
    }
    if (rx >= 0) {
        ESP_LOGI(TAG, "GPS: NMEA on GPIO %d @ %d baud", rx, baud);   // gps=1 in WD:CAPS once it has a fix, as always
    } else if (board_pin_busy(CONFIG_WARDRIVEESP_GPS_RX_GPIO)) {
        ESP_LOGI(TAG, "GPS: GPIO %d belongs to this board's own hardware, GPS off", CONFIG_WARDRIVEESP_GPS_RX_GPIO);
        uart_driver_delete(port);
        s_gps_task = NULL;
        vTaskDelete(NULL);
        return;
    } else {
        rx = awok ? AWOK_GPS_RX : CONFIG_WARDRIVEESP_GPS_RX_GPIO;   // nothing yet: keep listening where a module is wired
        if (awok) baud = 115200;
        ESP_LOGI(TAG, "GPS: no NMEA heard, listening on GPIO %d @ %d baud", rx, baud);
    }
    uart_set_baudrate(port, (uint32_t)baud);
    uart_set_pin(port, tx >= 0 ? tx : UART_PIN_NO_CHANGE, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    char line[128]; int li = 0;
    uint8_t buf[128];
    uint32_t last_emit = 0;
    for (;;) {
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') { if (li > 0) { line[li] = '\0'; handle_nmea(line); li = 0; } }
            else if (li < (int)sizeof(line) - 1) line[li++] = c;
            else li = 0;                           // oversized/garbled line — drop it
        }
        // Emit at most once a second, only while connected (send_line no-ops when there's no link).
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_gps_dirty && now - last_emit >= 1000) {
            s_gps_dirty = false; last_emit = now;
            char latb[20], lonb[20], altb[16];
            fmt_fixed(latb, sizeof(latb), s_gps_lat, 1000000, 6);
            fmt_fixed(lonb, sizeof(lonb), s_gps_lon, 1000000, 6);
            if (s_gps_has_alt) {
                fmt_fixed(altb, sizeof(altb), s_gps_alt, 10, 1);
                send_line("WD:GPS ts=%lu lat=%s lon=%s alt=%s fix=1 sats=%d",
                          (unsigned long)now, latb, lonb, altb, s_gps_sats);
            } else {
                send_line("WD:GPS ts=%lu lat=%s lon=%s fix=1 sats=%d",
                          (unsigned long)now, latb, lonb, s_gps_sats);
            }
        }
    }
}
#endif  // CONFIG_WARDRIVEESP_GPS_ENABLE
#if !CONFIG_WARDRIVEESP_GPS_ENABLE
static void gps_where(sdrun_ap_t *r) { r->fix = false; }
#endif

static void scan_one_channel(uint8_t ch) {
    wifi_scan_config_t cfg = { .channel = ch, .show_hidden = true };
    if (is_5ghz(ch) || is_dfs(ch)) {
        cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        cfg.scan_time.passive = PASSIVE_DWELL_MS;
    } else {
        cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;   // default dwell; coex rejects custom active times
    }
    s_crumb_ch = ch;
    s_crumb = CR_SCAN;
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) { s_crumb = CR_IDLE; return; }
    s_crumb = CR_RESULTS;
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK || found == 0) { s_crumb = CR_IDLE; return; }
    if (found > MAX_AP_PER_SCAN) found = MAX_AP_PER_SCAN;
    wifi_ap_record_t *aps = calloc(found, sizeof(*aps));
    if (!aps) return;
    s_crumb = CR_SEND_AP;
    if (esp_wifi_scan_get_ap_records(&found, aps) == ESP_OK)
        for (uint16_t i = 0; i < found; i++) send_ap_line(&aps[i]);
    free(aps);
    s_crumb = CR_IDLE;
}

static char s_boot_line[220];   // built at boot by boot_diag_begin()
static char s_crash_line[200];  // WD:CRASH for the previous run, when it ended in a crash or watchdog reset

static volatile uint32_t s_conns = 0, s_last_disc_reason = 0, s_last_disc_ms = 0;
static volatile uint32_t s_conn_since_ms = 0;   // when the current Bluetooth link came up

static void send_link_line(uint8_t sinks) {
    uint16_t mtu = s_bt_up && s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn_handle) : 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_last_disc_ms)
        send_line_to(sinks, "WD:LINK conns=%lu mtu=%u prev_disc=0x%03lx prev_disc_ago=%lus", (unsigned long)s_conns,
                     (unsigned)mtu, (unsigned long)s_last_disc_reason, (unsigned long)((now - s_last_disc_ms) / 1000));
    else
        send_line_to(sinks, "WD:LINK conns=%lu mtu=%u prev_disc=none", (unsigned long)s_conns, (unsigned)mtu);
}

/// Ask the phone for link timing inside Apple's accessory guidelines. Normal: 30-50 ms interval, no latency, 5 s
/// supervision timeout. A phone holding many boards fits them into its radio schedule more easily, and a board busy on
/// Wi-Fi (radio coexistence) can miss a few connection events without the link dropping. Scan lines need far less than
/// this carries at a 247-byte MTU. A firmware update over BLE asks for 15-30 ms while it runs. The phone may refuse.
static void request_link_params(bool fast) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_upd_params p = {
        .itvl_min = fast ? 12 : 24,      // 1.25 ms units: 15 / 30 ms
        .itvl_max = fast ? 24 : 40,      // 30 / 50 ms
        .latency = 0,
        .supervision_timeout = 500,      // 10 ms units: 5 s
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(s_conn_handle, &p);
    if (rc != 0) ESP_LOGW(TAG, "link timing request failed: %d", rc);
}

/// The link timing in use after the phone answered a request (or changed it on its own).
static void send_conn_line(int status) {
    struct ble_gap_conn_desc d;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(s_conn_handle, &d) != 0) return;
    ESP_LOGI(TAG, "link timing: status %d, interval %u x 1.25 ms, latency %u, timeout %u x 10 ms", status,
             (unsigned)d.conn_itvl, (unsigned)d.conn_latency, (unsigned)d.supervision_timeout);
    send_line_to(SINK_BLE, "WD:CONN itvl_ms=%u latency=%u timeout_ms=%u status=%d", (unsigned)(d.conn_itvl * 5 / 4),
                 (unsigned)d.conn_latency, (unsigned)d.supervision_timeout * 10, status);
}

static void send_header(uint8_t sinks) {
    send_caps_to(sinks);
    if (s_boot_line[0]) send_line_to(sinks, "%s", s_boot_line);
    if (s_crash_line[0]) send_line_to(sinks, "%s", s_crash_line);
    send_link_line(sinks);
    if (sdrun_mounted()) {   // closed offline runs the app can take (see "Offline runs")
        uint32_t n, bytes;
        sdrun_pending(&n, &bytes);
        send_line_to(sinks, "WD:RUNS n=%lu bytes=%lu", (unsigned long)n, (unsigned long)bytes);
    }
}

static void scan_task(void *arg) {
    (void)arg;
    send_header(SINK_ALL);
    send_line("WD:MODE mode=wifi");
    send_line("WD:BEGIN type=wifi interval=0 channel=auto");
    send_plan_line();
    s_sweeps = 0;
    uint8_t plan[MAX_CHANNELS];
    while (s_streaming && scan_wanted()) {
        size_t n = plan_copy(plan);
        s_ap_count = 0;   // APs heard this sweep (one record per AP per channel scan)
        for (size_t i = 0; i < n && s_streaming; i++) {
            scan_one_channel(plan[i]);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (n == 0) vTaskDelay(pdMS_TO_TICKS(200));
        s_sweeps++;
        heap_check("wifi sweep");
        s_last_aps = s_ap_count;
        send_line("WD:STATUS aps=%lu scans=%lu", (unsigned long)s_ap_count, (unsigned long)s_sweeps);
    }
    send_line("WD:END reason=stop");
    s_streaming = false;
    s_scan_task = NULL;
    if (s_switch_pending) { s_switch_pending = false; stream_start(); }
    vTaskDelete(NULL);
}

static void wifi_start(void) {
    if (s_streaming || s_scan_task || s_ota_active || !scan_wanted()) return;
    s_streaming = true;
    if (xTaskCreate(scan_task, "wd_scan", 10240, NULL, 5, &s_scan_task) != pdPASS) {
        s_streaming = false;
        s_scan_task = NULL;
        ESP_LOGE(TAG, "scan task create failed");
        return;
    }
    ESP_LOGI(TAG, "stream started");
}

// ─── Capture role (PROTOCOL.md "Capture", firmware 1.5+) ──────────────────────
// PASSIVE 802.11 promiscuous capture: WPA 4-way handshakes (WD:HS, hashcat-22000), RSN PMKIDs (WD:PMKID) and
// station sightings (WD:CLIENT). RX ONLY — the board never transmits, deauths or injects; a crackable artifact
// only appears when a client associates on its own. A C port of the Android PmkidCapture (EAPOL M1..M4, ANonce/M2
// replay pairing, MIC-zeroed hashcat line, PMKID KDE) plus MonitorCapture's client/BSSID derivation. The
// promiscuous rx callback runs in the Wi-Fi task and must not block, so it copies each frame (truncated) into a
// queue; cap_task drains it and does all the parsing.
#define CAP_SNAP        256   // bytes copied per frame — EAPOL key frames are ~155 B, SSID sits near a beacon's front
#define CAP_QUEUE_LEN   32
#define CAP_DWELL_MS    300   // longer than a scan dwell: a handshake only happens if a client associates while we camp
#define CAP_M1_CACHE    16    // M1 ANonces awaiting their M2
#define CAP_AP_CACHE    160   // BSSID→SSID/best-RSSI table: feeds WD:AP so capture is a superset of a Wi-Fi scan
#define CAP_SEEN_CAP    512   // session dedup fingerprints (handshake MIC / PMKID / client) before it rolls over

typedef struct {
    uint16_t len;
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  data[CAP_SNAP];
} cap_rec_t;

static QueueHandle_t s_cap_queue;
static TaskHandle_t  s_cap_task        = NULL;
static volatile bool s_cap_running     = false;
static volatile uint32_t s_cap_hs      = 0;
static volatile uint32_t s_cap_pmkid   = 0;
static volatile uint32_t s_cap_clients = 0;
static volatile uint32_t s_cap_aps     = 0;
static volatile uint32_t s_cap_data    = 0;
static volatile uint32_t s_cap_dropped = 0;

typedef struct {   // M1 ANonce cache (raw bytes), keyed AP+STA, paired with the matching M2 by replay counter
    bool     used;
    uint8_t  ap[6], sta[6], anonce[32], replay[8];
    uint32_t time;
} cap_m1_t;
static cap_m1_t s_cap_m1[CAP_M1_CACHE];

typedef struct { bool used; uint8_t bssid[6], ssid[32], ssid_len; int8_t best_rssi; uint32_t last_emit; } cap_ap_t;
static cap_ap_t s_cap_aps_tbl[CAP_AP_CACHE];
#define CAP_AP_REEMIT_MS 20000   // re-report a still-heard AP at least this often so the phone's "in range" count stays live

static uint32_t s_cap_seen[CAP_SEEN_CAP];   // open set of 32-bit artifact fingerprints; cleared when full
static size_t   s_cap_seen_n = 0;

static uint32_t cap_fnv(const uint8_t *p, size_t n, uint32_t seed) {
    uint32_t h = seed ? seed : 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static bool cap_seen_add(uint32_t fp) {
    for (size_t i = 0; i < s_cap_seen_n; i++) if (s_cap_seen[i] == fp) return false;
    if (s_cap_seen_n >= CAP_SEEN_CAP) s_cap_seen_n = 0;   // roll over; the phone dedups by pmkid/mic too
    s_cap_seen[s_cap_seen_n++] = fp;
    return true;
}
static void cap_reset_session(void) {
    memset(s_cap_m1, 0, sizeof(s_cap_m1));
    memset(s_cap_aps_tbl, 0, sizeof(s_cap_aps_tbl));
    s_cap_seen_n = 0;
    s_cap_hs = s_cap_pmkid = s_cap_clients = s_cap_aps = s_cap_data = s_cap_dropped = 0;
}

static int cap_hex(char *out, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { out[2 * i] = lower_hex_digit(p[i] >> 4); out[2 * i + 1] = lower_hex_digit(p[i] & 0x0F); }
    out[2 * n] = '\0';
    return (int)(2 * n);
}

// Upsert a beacon/probe-response's BSSID into the AP table (SSID + best RSSI). Returns true when the phone should
// get a WD:AP for it — the first time it's seen, or when a later sighting is notably stronger — so capture mode
// reports APs like a Wi-Fi scan without flooding the link with one line per beacon. Evicts the weakest when full.
static bool cap_ap_touch(const uint8_t *bssid, const uint8_t *ssid, int ssid_len, int rssi) {
    int slot = -1, freeslot = -1, weakest = 0; int8_t weakest_rssi = 127;
    for (int i = 0; i < CAP_AP_CACHE; i++) {
        if (s_cap_aps_tbl[i].used && memcmp(s_cap_aps_tbl[i].bssid, bssid, 6) == 0) { slot = i; break; }
        if (!s_cap_aps_tbl[i].used && freeslot < 0) freeslot = i;
        if (s_cap_aps_tbl[i].used && s_cap_aps_tbl[i].best_rssi < weakest_rssi) { weakest_rssi = s_cap_aps_tbl[i].best_rssi; weakest = i; }
    }
    bool fresh = (slot < 0);
    if (fresh) {
        slot = (freeslot >= 0) ? freeslot : weakest;
        s_cap_aps_tbl[slot].used = true;
        memcpy(s_cap_aps_tbl[slot].bssid, bssid, 6);
        s_cap_aps_tbl[slot].ssid_len = 0;
        s_cap_aps_tbl[slot].best_rssi = -128;
    }
    cap_ap_t *a = &s_cap_aps_tbl[slot];
    if (ssid_len > 0 && ssid_len <= 32) { memcpy(a->ssid, ssid, ssid_len); a->ssid_len = (uint8_t)ssid_len; }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // Emit on first sight, a notably stronger RSSI, or if it's been a while — so the phone keeps counting it "in range".
    if (fresh || rssi > (int)a->best_rssi + 4 || now - a->last_emit > CAP_AP_REEMIT_MS) {
        if (rssi > (int)a->best_rssi) a->best_rssi = (int8_t)rssi;
        a->last_emit = now;
        return true;
    }
    return false;
}
static const cap_ap_t *cap_find_ssid(const uint8_t *bssid) {
    for (int i = 0; i < CAP_AP_CACHE; i++)
        if (s_cap_aps_tbl[i].used && memcmp(s_cap_aps_tbl[i].bssid, bssid, 6) == 0 && s_cap_aps_tbl[i].ssid_len)
            return &s_cap_aps_tbl[i];
    return NULL;
}

// The WPA/WPA2/WPA3 token for a beacon/probe-response, from its capability + RSN/WPA information elements — the
// same token set a Wi-Fi scan reports (auth_name), so capture-mode WD:AP lines read identically to scan-mode ones.
static const char *cap_beacon_auth(const uint8_t *b, int end) {
    if (end < 36) return "OPEN";
    int capinfo = b[34] | (b[35] << 8);
    if (!(capinfo & 0x10)) return "OPEN";                       // Privacy bit clear → open
    bool rsn = false, wpa = false, sae = false, psk = false;
    for (int i = 36; i + 2 <= end; ) {
        int id = b[i], len = b[i + 1];
        if (i + 2 + len > end) break;
        const uint8_t *p = b + i + 2;
        if (id == 0x30 && len >= 8) {                           // RSN IE
            rsn = true;
            int off = 2 + 4;                                    // skip version + group cipher
            if (off + 2 <= len) {
                int pc = p[off] | (p[off + 1] << 8); off += 2 + 4 * pc;   // skip pairwise ciphers
                if (off + 2 <= len) {
                    int ac = p[off] | (p[off + 1] << 8); off += 2;
                    for (int k = 0; k < ac && off + 4 <= len; k++, off += 4) {
                        if (p[off] == 0x00 && p[off + 1] == 0x0F && p[off + 2] == 0xAC) {
                            uint8_t suite = p[off + 3];
                            if (suite == 8 || suite == 9) sae = true;    // SAE / FT-SAE → WPA3
                            if (suite == 2 || suite == 6) psk = true;    // PSK / FT-PSK
                        }
                    }
                }
            }
        } else if (id == 0xDD && len >= 4 && p[0] == 0x00 && p[1] == 0x50 && p[2] == 0xF2 && p[3] == 0x01) {
            wpa = true;                                         // Microsoft WPA1 vendor IE
        }
        i += 2 + len;
    }
    if (sae) return (rsn && psk) ? "WPA2_WPA3" : "WPA3";
    if (rsn) return "WPA2";
    if (wpa) return "WPA";
    return "WEP";                                               // privacy set but no RSN/WPA IE → WEP
}

// Emit a WD:AP line from a beacon/probe-response, identical in shape to the scan-mode AP line.
static void cap_emit_ap(const uint8_t *b, const uint8_t *ssid, int ssid_len, int rssi, int ch, const char *auth) {
    display_note_ap(b, (uint8_t)ch);
    lcd_note_ap(b, (uint8_t)ch);
    char ssid_hex[2 * 32 + 1] = "";
    if (ssid_len > 0 && ssid_len <= 32) cap_hex(ssid_hex, ssid, (size_t)ssid_len);
    send_line("WD:AP ts=%lu bssid=%02X:%02X:%02X:%02X:%02X:%02X ssid_hex=%s rssi=%d ch=%d auth=%s hidden=%u",
              (unsigned long)(esp_timer_get_time() / 1000), b[0], b[1], b[2], b[3], b[4], b[5],
              ssid_hex, rssi, ch, auth, ssid_len ? 0u : 1u);
    s_cap_aps++;
}

// A station sighting, deduped once per capture session. Broadcast/multicast/zero MACs are not devices.
static void cap_emit_client(const uint8_t *mac, const uint8_t *bssid, int rssi, int ch) {
    if (mac[0] & 0x01) return;
    bool zero = true; for (int i = 0; i < 6; i++) if (mac[i]) zero = false;
    if (zero) return;
    uint32_t fp = cap_fnv(mac, 6, 0x515ad70bu);
    if (!cap_seen_add(fp)) return;
    char bss[20] = "";
    if (bssid) snprintf(bss, sizeof(bss), "%02X:%02X:%02X:%02X:%02X:%02X",
                        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    send_line("WD:CLIENT ts=%lu mac=%02X:%02X:%02X:%02X:%02X:%02X bssid=%s rssi=%d ch=%d",
              (unsigned long)(esp_timer_get_time() / 1000), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
              bss, rssi, ch);
    s_cap_clients++;
}

// Port of PmkidCapture.scan: locate EAPOL in a data frame, classify M1..M4, pair M1/M2 into a hashcat-22000
// handshake line, and lift the RSN PMKID KDE. `buf` is the 802.11 frame at offset 0; `end` excludes the FCS.
static void cap_parse_eapol(const uint8_t *buf, int end, int ch, int rssi) {
    if (end < 2 || ((buf[0] >> 2) & 3) != 2) return;          // data frames only
    int fromDs = (buf[1] >> 1) & 1;
    int p = -1, hi = end - 8; if (hi > 40) hi = 40;           // scan the header/pad window for AA AA 03 .. .. .. 88 8E
    for (int q = 24; q <= hi; q++)
        if (buf[q] == 0xAA && buf[q + 1] == 0xAA && buf[q + 2] == 0x03 && buf[q + 6] == 0x88 && buf[q + 7] == 0x8E) { p = q; break; }
    if (p < 0) return;
    int eapol = p + 8;
    if (eapol + 7 > end || buf[eapol + 1] != 3) return;       // EAPOL-Key
    int keyInfo = (buf[eapol + 5] << 8) | buf[eapol + 6];
    bool ack = keyInfo & 0x080, hasMic = keyInfo & 0x100, install = keyInfo & 0x040, secure = keyInfo & 0x200;
    int m = (ack && !hasMic) ? 1 : (hasMic && !ack && !secure && !install) ? 2
          : (hasMic && ack && install) ? 3 : (hasMic && secure && !ack) ? 4 : 0;
    const uint8_t *ap  = fromDs ? buf + 10 : buf + 4;         // M1: AP=addr2; M2: AP=addr1
    const uint8_t *sta = fromDs ? buf + 4  : buf + 10;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (m == 1 && eapol + 49 <= end) {                        // cache the ANonce for M2 pairing
        int slot = -1, freeslot = -1, oldest = 0; uint32_t oldest_t = 0xffffffffu;
        for (int i = 0; i < CAP_M1_CACHE; i++) {
            if (s_cap_m1[i].used && !memcmp(s_cap_m1[i].ap, ap, 6) && !memcmp(s_cap_m1[i].sta, sta, 6)) { slot = i; break; }
            if (!s_cap_m1[i].used && freeslot < 0) freeslot = i;
            if (s_cap_m1[i].time < oldest_t) { oldest_t = s_cap_m1[i].time; oldest = i; }
        }
        if (slot < 0) slot = (freeslot >= 0) ? freeslot : oldest;
        s_cap_m1[slot].used = true;
        memcpy(s_cap_m1[slot].ap, ap, 6); memcpy(s_cap_m1[slot].sta, sta, 6);
        memcpy(s_cap_m1[slot].anonce, buf + eapol + 17, 32);
        memcpy(s_cap_m1[slot].replay, buf + eapol + 9, 8);
        s_cap_m1[slot].time = now;
    }
    if (m == 2 && eapol + 99 <= end) {                        // pair with the cached M1 by replay counter
        int bodyLen = (buf[eapol + 2] << 8) | buf[eapol + 3];
        int frameLen = 4 + bodyLen;
        cap_m1_t *m1 = NULL;
        for (int i = 0; i < CAP_M1_CACHE; i++)
            if (s_cap_m1[i].used && !memcmp(s_cap_m1[i].ap, ap, 6) && !memcmp(s_cap_m1[i].sta, sta, 6)) { m1 = &s_cap_m1[i]; break; }
        if (m1 && !memcmp(m1->replay, buf + eapol + 9, 8) && frameLen >= 99 && eapol + frameLen <= end) {
            const uint8_t *mic = buf + eapol + 81;
            bool mic_zero = true; for (int i = 0; i < 16; i++) if (mic[i]) mic_zero = false;
            if (!mic_zero && cap_seen_add(cap_fnv(mic, 16, 0x48530001u))) {
                if (frameLen > CAP_SNAP) frameLen = CAP_SNAP;
                uint8_t fr[CAP_SNAP];
                memcpy(fr, buf + eapol, frameLen);
                for (int k = 81; k < 97 && k < frameLen; k++) fr[k] = 0;   // zero the MIC field in the EAPOL copy
                char michex[33], anoncehex[65], eapolhex[2 * CAP_SNAP + 1], essidhex[2 * 32 + 1] = "";
                cap_hex(michex, mic, 16);
                cap_hex(anoncehex, m1->anonce, 32);
                cap_hex(eapolhex, fr, frameLen);
                const cap_ap_t *ss = cap_find_ssid(ap);
                if (ss) cap_hex(essidhex, ss->ssid, ss->ssid_len);
                // Worst case (256-byte EAPOL → 512 hex, 64-byte ESSID, 64 ANonce) reaches ~880 bytes; size for it.
                // send_chunks splits it across MTU-sized DATA frames, so only this build buffer caps the length.
                char line[1100];
                int n = snprintf(line, sizeof(line) - 1,
                    "WD:HS ts=%lu bssid=%02X:%02X:%02X:%02X:%02X:%02X sta=%02X:%02X:%02X:%02X:%02X:%02X ssid_hex=%s ch=%d rssi=%d "
                    "hc22000=WPA*02*%s*%02x%02x%02x%02x%02x%02x*%02x%02x%02x%02x%02x%02x*%s*%s*%s*00",
                    (unsigned long)now, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5],
                    sta[0], sta[1], sta[2], sta[3], sta[4], sta[5], essidhex, ch, rssi,
                    michex, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5],
                    sta[0], sta[1], sta[2], sta[3], sta[4], sta[5], essidhex, anoncehex, eapolhex);
                if (n > 0) {
                    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
                    line[n++] = '\n';
                    send_chunks(line, (size_t)n);
                    s_cap_hs++;
                    m1->used = false;
                }
            }
        }
    }
    for (int i = eapol, limit = end - 22; i <= limit; i++) {  // PMKID KDE: DD 14 00 0F AC 04 <16 bytes>
        if (buf[i] == 0xDD && buf[i + 1] == 0x14 && buf[i + 2] == 0x00 && buf[i + 3] == 0x0F &&
            buf[i + 4] == 0xAC && buf[i + 5] == 0x04) {
            const uint8_t *pm = buf + i + 6;
            bool zero = true; for (int k = 0; k < 16; k++) if (pm[k]) zero = false;
            if (zero) return;
            if (!cap_seen_add(cap_fnv(pm, 16, 0x504d4b31u))) return;
            char pmhex[33], essidhex[2 * 32 + 1] = "";
            cap_hex(pmhex, pm, 16);
            const cap_ap_t *ss = cap_find_ssid(ap);
            if (ss) cap_hex(essidhex, ss->ssid, ss->ssid_len);
            send_line("WD:PMKID ts=%lu bssid=%02X:%02X:%02X:%02X:%02X:%02X sta=%02X:%02X:%02X:%02X:%02X:%02X "
                      "ssid_hex=%s ch=%d rssi=%d pmkid=%s",
                      (unsigned long)now, ap[0], ap[1], ap[2], ap[3], ap[4], ap[5],
                      sta[0], sta[1], sta[2], sta[3], sta[4], sta[5], essidhex, ch, rssi, pmhex);
            s_cap_pmkid++;
            return;
        }
    }
}

static void cap_process(const cap_rec_t *r) {
    const uint8_t *b = r->data;
    int end = r->len;
    if (end < 24) return;                                     // a full MAC header is 24 bytes
    int fc = b[0], ftype = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
    if (ftype == 0) {                                         // management
        const uint8_t *addr2 = b + 10, *addr3 = b + 16;       // transmitter / BSSID
        if (sub == 8 || sub == 5) {                           // beacon / probe-response → AP (WD:AP) + learn SSID
            const uint8_t *ssid = NULL; int sl = 0;
            int t = 24 + 12;                                  // after 24-byte header + 12 fixed params
            if (t + 2 <= end && b[t] == 0) {                  // element id 0 = SSID
                int l = b[t + 1];
                if (l > 0 && l <= 32 && t + 2 + l <= end) { ssid = b + t + 2; sl = l; }
            }
            apc_add(addr3, (uint32_t)(esp_timer_get_time() / 1000));   // every frame: the live count stays live
            if (cap_ap_touch(addr3, ssid, sl, r->rssi))
                cap_emit_ap(addr3, ssid, sl, r->rssi, r->channel, cap_beacon_auth(b, end));
        } else if (sub == 4) {
            cap_emit_client(addr2, NULL, r->rssi, r->channel);          // probe-request — client, BSSID unknown
        } else if (sub <= 3 || sub == 11) {
            cap_emit_client(addr2, addr3, r->rssi, r->channel);         // assoc/reassoc/auth — client + BSSID
        }
    } else if (ftype == 2) {                                  // data
        s_cap_data++;
        cap_parse_eapol(b, end, r->channel, r->rssi);
        int fromDs = (b[1] >> 1) & 1, toDs = b[1] & 1;
        const uint8_t *a1 = b + 4, *a2 = b + 10;
        if (toDs && !fromDs)       cap_emit_client(a2, a1, r->rssi, r->channel);   // client→AP: client=addr2 bssid=addr1
        else if (!toDs && fromDs)  cap_emit_client(a1, a2, r->rssi, r->channel);   // AP→client: client=addr1 bssid=addr2
        else if (!toDs && !fromDs) cap_emit_client(a2, a1, r->rssi, r->channel);   // IBSS
        // WDS (toDs && fromDs): four addresses, no simple client/BSSID — skip
    }
}

static void cap_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_cap_running || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    int end = pkt->rx_ctrl.sig_len - 4;                       // drop the trailing FCS
    if (end <= 0) return;
    cap_rec_t r;
    r.len = end > CAP_SNAP ? CAP_SNAP : (uint16_t)end;
    r.rssi = pkt->rx_ctrl.rssi;
    r.channel = pkt->rx_ctrl.channel;
    memcpy(r.data, pkt->payload, r.len);
    QueueHandle_t q = s_cap_queue;
    if (!q || xQueueSend(q, &r, 0) != pdTRUE) s_cap_dropped++;
}

static void cap_task(void *arg) {
    (void)arg;
    s_crumb = CR_CAP;
    cap_reset_session();
    send_caps_line();
    send_line("WD:MODE mode=cap");
    send_line("WD:BEGIN type=cap interval=0 channel=auto");
    send_plan_line();

    (void)esp_wifi_scan_stop();
    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(cap_rx_cb);
    if (esp_wifi_set_promiscuous(true) != ESP_OK) send_line("WD:ERR reason=promisc_failed");

    uint8_t plan[MAX_CHANNELS];
    uint32_t sweeps = 0;
    cap_rec_t r;
    while (s_cap_running && host_linked()) {
        size_t n = plan_copy(plan);
        if (n == 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        for (size_t i = 0; i < n && s_cap_running; i++) {
            esp_wifi_set_channel(plan[i], WIFI_SECOND_CHAN_NONE);
            s_crumb_ch = plan[i];
            uint32_t until = (uint32_t)(esp_timer_get_time() / 1000) + CAP_DWELL_MS;
            while ((uint32_t)(esp_timer_get_time() / 1000) < until && s_cap_running)
                if (xQueueReceive(s_cap_queue, &r, pdMS_TO_TICKS(20)) == pdTRUE) cap_process(&r);
        }
        sweeps++;
        heap_check("cap sweep");
        send_line("WD:STATUS cap=1 aps=%lu hs=%lu pmkid=%lu clients=%lu data=%lu scans=%lu dropped=%lu",
                  (unsigned long)s_cap_aps, (unsigned long)s_cap_hs, (unsigned long)s_cap_pmkid,
                  (unsigned long)s_cap_clients, (unsigned long)s_cap_data, (unsigned long)sweeps,
                  (unsigned long)s_cap_dropped);
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    // Process frames queued before the stop. A handshake still missing its M2 is not reported.
    for (int i = 1; xQueueReceive(s_cap_queue, &r, 0) == pdTRUE; i++) {
        cap_process(&r);
        if (i % 4 == 0) vTaskDelay(1);
    }
    send_line("WD:END reason=stop");
    s_cap_running = false;
    s_cap_task = NULL;
    if (s_switch_pending) { s_switch_pending = false; stream_start(); }
    vTaskDelete(NULL);
}

static void cap_start(void) {
    if (s_cap_running || s_cap_task || s_ota_active || !host_linked()) return;
    if (!s_cap_queue) s_cap_queue = xQueueCreate(CAP_QUEUE_LEN, sizeof(cap_rec_t));
    if (!s_cap_queue) { send_line("WD:ERR reason=no_memory"); return; }
    s_cap_running = true;
    if (xTaskCreate(cap_task, "wd_cap", 10240, NULL, 5, &s_cap_task) != pdPASS) {
        // Heap too fragmented for the 10 KB stack beside a full queue (nothing feeds the queue until the task turns
        // promiscuous mode on): try once more with a half-size queue.
        vQueueDelete(s_cap_queue);
        s_cap_queue = xQueueCreate(CAP_QUEUE_LEN / 2, sizeof(cap_rec_t));
        if (s_cap_queue && xTaskCreate(cap_task, "wd_cap", 10240, NULL, 5, &s_cap_task) == pdPASS) {
            ESP_LOGW(TAG, "capture started with a half-size queue (low memory)");
            return;
        }
        s_cap_running = false;
        s_cap_task = NULL;
        send_line("WD:ERR reason=no_task");
        return;
    }
    ESP_LOGI(TAG, "capture started");
}

// ─── Wi-Fi radar: CSI (PROTOCOL.md "Wi-Fi radar (CSI)", experimental) ──────
// Channel State Information is the per-subcarrier channel estimate of every frame the radio decodes. People moving
// between a transmitter and this board change it, which the app turns into motion and presence. Opt-in only (`csi
// start` / `csi tx` / `csi mesh`), never part of a scan, and it ends with the link like every other role.
//
// The Wi-Fi driver only does CSI when it was initialized with csi_enable, which costs about 1 KB per static RX buffer
// (~10 KB). So the board keeps booting exactly as before (CSI off, CONFIG_ESP_WIFI_CSI_ENABLED off), and this role
// restarts Wi-Fi with CSI on when it starts and restarts it again exactly as booted when it ends. If that last restart
// ever fails the board reboots rather than staying without Wi-Fi.
//
// rx (`csi start`): promiscuous on one channel, CSI callback in the Wi-Fi task. The callback filters by transmitter,
// limits the rate, reduces each frame to `sc` 8-bit amplitudes (RMS of each group of subcarriers) and queues it;
// csi_task batches them into WD:CSI lines.
// tx (`csi tx`): the board sends a small data frame at a steady rate at HT20 MCS0 (an OFDM rate, so receivers get an
// HT-LTF estimate) to a WardriveESP group address that carries its id and slot.
// mesh (`csi mesh`, radio tomography with several boards): every board transmits like tx and receives the others. The
// callback folds each frame from another board into that link's window (count, RSSI mean/spread, 8 normalized
// amplitude bins and their spread); csi_task sends every link's window as one WD:CSIMESH line per report period. With
// slots, board slot 0 free-runs and the others time their frames from its frames (CSMA alone is usually enough).
static volatile bool s_csi_running = false;
static TaskHandle_t  s_csi_task    = NULL;
static volatile bool s_wifi_restarting = false;   // csi_wifi_restart has the driver down: nothing else touches it
static volatile uint32_t s_csi_pps = 0;            // packets (rx) or links (mesh) or frames (tx) last second, for the beacon

#if CSI_SUPPORTED
static volatile scan_role_t s_csi_prev = ROLE_WIFI;   // the role `csi stop` returns to

#define CSI_SC_MAX     64     // amplitudes per packet, upper bound of `sc=`
#define CSI_QUEUE_LEN  12
#define CSI_BATCH_MAX  8      // packets per WD:CSI line
#define CSI_BATCH_MS   100    // a batch never waits longer than this
#define CSI_PKT_MAX    (40 + 2 * CSI_SC_MAX)   // one packet's text, worst case
#define CSI_LINE_MAX   (64 + CSI_BATCH_MAX * CSI_PKT_MAX)
#define CSI_SRC_MAX    16     // transmitters announced with WD:CSISRC per session before the table starts over
#define MESH_BINS      8      // amplitude bins per mesh link
#define MESH_LINKS     16     // transmitters a mesh board keeps windows for
#define MESH_LINK_MAX  48     // one link's text in WD:CSIMESH, worst case
#define MESH_REF_MS    2000   // slot timing follows slot 0's frames heard within this long, else free-runs

// Our frames go to a locally administered group address: 03:57:44:43 ("\x03WDC"), then the sender's slot and id. Any
// board in promiscuous mode sees them; a normal station ignores them.
static const uint8_t CSI_GROUP[4] = { 0x03, 0x57, 0x44, 0x43 };

typedef enum { CSI_RX = 0, CSI_TX, CSI_MESH } csi_mode_t;
typedef struct {
    csi_mode_t mode;
    uint8_t ch;
    bool    has_mac;          // rx: only frames from this transmitter
    uint8_t mac[6];
    uint8_t rate;             // rx: most packets per second sent on; tx and mesh: frames per second sent
    uint8_t sc;               // rx: amplitudes per packet
    uint8_t id;               // tx and mesh: this board's id, carried in its frames
    uint8_t slot, slots;      // tx and mesh: this board's slot of `slots` (1 = no slots)
    uint16_t report_ms;       // mesh: one WD:CSIMESH line this often
    bool    raw;              // tx and mesh: send raw 802.11 frames (`via=raw`) instead of ESP-NOW (`via=espnow`)
} csi_cfg_t;
static csi_cfg_t s_csi_cfg = { .mode = CSI_RX, .ch = 6, .rate = 30, .sc = 32, .slots = 1, .report_ms = 100 };
static const char *csi_mode_name(csi_mode_t m) { return m == CSI_TX ? "tx" : m == CSI_MESH ? "mesh" : "rx"; }

typedef struct {
    uint32_t ms;              // since boot, when the callback ran
    int8_t   rssi, nf;        // dBm, and the noise floor as the driver reports it
    uint8_t  fmt;             // rx_ctrl.cur_bb_format: 1 11a/g, 2 HT, 3 VHT, 4 HE SU, ...
    int16_t  id;              // a WardriveESP transmitter's id, -1 for anyone else
    uint16_t nsc;             // subcarriers in the driver's buffer (its length / 2)
    uint8_t  mac[6];          // transmitter
    uint8_t  amp[CSI_SC_MAX];
} csi_rec_t;

typedef struct { uint16_t hash; uint8_t mac[6]; } csi_src_t;

// One mesh link's window (filled by the callback under s_mesh_lock, taken and cleared by csi_task).
typedef struct {
    bool     used;
    uint8_t  id;
    uint8_t  mac[6];
    uint16_t n;
    int32_t  rssi_sum;
    uint32_t rssi_sq;
    uint32_t a_sum[MESH_BINS], a_sq[MESH_BINS];   // normalized bins: 128 = this packet's mean
} mesh_link_t;

static QueueHandle_t s_csi_queue;
// Read by the callback (Wi-Fi task) and the TX timer; set by csi_task before CSI is switched on.
static volatile bool s_csi_rx_on = false;
static csi_mode_t s_csi_mode;
static bool     s_csi_filter;
static uint8_t  s_csi_filter_mac[6];
static uint8_t  s_csi_k = 32;
static int64_t  s_csi_period_us, s_csi_next_us;
static volatile uint32_t s_csi_heard, s_csi_skipped, s_csi_dropped;   // totals for this session
static volatile bool s_csi_resend_src = false;   // a host joined: announce the transmitters again
static mesh_link_t *volatile s_mesh;              // MESH_LINKS windows, mesh mode only
static portMUX_TYPE s_mesh_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t  s_mesh_id;
// Transmit side (tx and mesh): a one-shot esp_timer re-armed for every frame.
static esp_timer_handle_t s_tx_timer;
static volatile bool s_tx_on = false;
static uint8_t  s_tx_frame[24 + 8 + 13];          // via=raw
static uint8_t  s_tx_now[24];                     // via=espnow: the ESP-NOW payload
static bool     s_tx_raw, s_espnow_on;
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static char     s_txrate[32] = "none";           // WD:CSICFG txrate=: ok (HT20 MCS0), ofdm6 (fallback), or the error
static esp_err_t s_txrate_pre = ESP_ERR_NOT_FINISHED;   // via=raw: the rate call made before esp_wifi_start
static uint32_t s_tx_seq;
static int64_t  s_tx_period_us, s_tx_next_us;
static uint8_t  s_tx_slot, s_tx_slots;
static volatile int64_t s_slot_ref_us;            // when slot 0's last frame was heard (mesh with slots)
static volatile uint32_t s_tx_sent, s_tx_fail;

static uint32_t csi_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

static uint16_t csi_src_hash(const uint8_t *mac) {
    uint32_t h = cap_fnv(mac, 6, 0x43534931u);
    return (uint16_t)(h ^ (h >> 16));
}

/// The driver's subcarriers in k equal groups, each the RMS magnitude of its nonzero I/Q values (at most ~181).
static void csi_bins(const wifi_csi_info_t *info, int k, uint8_t *out) {
    const int n = info->len / 2, first = info->first_word_invalid ? 2 : 0;
    const int8_t *b = info->buf;   // per subcarrier: imaginary, real (8-bit signed)
    for (int j = 0; j < k; j++) {
        int lo = j * n / k, hi = (j + 1) * n / k;
        if (hi <= lo) hi = lo + 1;
        uint32_t sum = 0, cnt = 0;
        for (int i = lo < first ? first : lo; i < hi && i < n; i++) {
            int im = b[2 * i], re = b[2 * i + 1];
            uint32_t p = (uint32_t)(im * im + re * re);
            if (p) { sum += p; cnt++; }   // guard and null subcarriers read 0
        }
        uint32_t a = cnt ? isqrt32(sum / cnt) : 0;
        out[j] = (uint8_t)(a > 255 ? 255 : a);
    }
}

// Mesh: fold one frame from another board into its link's window.
static void mesh_note(const wifi_csi_info_t *info, uint8_t id) {
    uint8_t a[MESH_BINS];
    csi_bins(info, MESH_BINS, a);
    uint32_t mean = 0;
    for (int j = 0; j < MESH_BINS; j++) mean += a[j];
    if (!mean) return;
    uint16_t norm[MESH_BINS];   // 128 = this packet's mean: the radio's gain steps cancel out
    for (int j = 0; j < MESH_BINS; j++) { uint32_t v = (uint32_t)a[j] * 128 * MESH_BINS / mean; norm[j] = (uint16_t)(v > 1023 ? 1023 : v); }
    int rssi = info->rx_ctrl.rssi;
    mesh_link_t *t = s_mesh;
    if (!t) return;
    portENTER_CRITICAL(&s_mesh_lock);
    mesh_link_t *l = NULL;
    for (int i = 0; i < MESH_LINKS && !l; i++) if (t[i].used && t[i].id == id) l = &t[i];
    for (int i = 0; i < MESH_LINKS && !l; i++) if (!t[i].used) { l = &t[i]; memset(l, 0, sizeof(*l)); l->used = true; l->id = id; }
    if (l && l->n < 3000) {   // 1023^2 x 3000 still fits the 32-bit sums
        memcpy(l->mac, info->mac, 6);
        l->n++;
        l->rssi_sum += rssi;
        l->rssi_sq += (uint32_t)(rssi * rssi);
        for (int j = 0; j < MESH_BINS; j++) { l->a_sum[j] += norm[j]; l->a_sq[j] += (uint32_t)norm[j] * norm[j]; }
    } else if (!l) {
        s_csi_dropped++;   // more transmitters than windows
    }
    portEXIT_CRITICAL(&s_mesh_lock);
}

/// A WardriveESP transmitter's frame: the raw frame's group address carries slot and id; either kind (raw or ESP-NOW)
/// carries "WDCSI", version, id, slot, slots in its body. The body offset differs per frame kind (LLC/SNAP, ESP-NOW
/// vendor element) and maybe per driver, so look for the magic near the start.
static bool csi_ours(const wifi_csi_info_t *info, uint8_t *id, uint8_t *slot) {
    if (!memcmp(info->dmac, CSI_GROUP, sizeof(CSI_GROUP))) { *slot = info->dmac[4]; *id = info->dmac[5]; return true; }
    const uint8_t *p = info->payload;
    int n = info->payload_len;
    if (!p || n < 13 || n > 1600) return false;
    int lim = n - 13;
    if (lim > 64) lim = 64;
    for (int i = 0; i <= lim; i++)
        if (p[i] == 'W' && !memcmp(p + i, "WDCSI", 5)) { *id = p[i + 6]; *slot = p[i + 7]; return true; }
    return false;
}

// Wi-Fi task: keep it short. rx: filter, rate-limit, reduce to amplitudes, queue. mesh: fold into the link window.
static void csi_rx_cb(void *ctx, wifi_csi_info_t *info) {
    (void)ctx;
    if (!s_csi_rx_on || !info || !info->buf || info->len < 8) return;
    uint8_t wid = 0, wslot = 0;
    bool ours = csi_ours(info, &wid, &wslot);   // a WardriveESP transmitter, with its id and slot
    int64_t now = esp_timer_get_time();
    if (s_csi_mode == CSI_MESH) {
        if (!ours || wid == s_mesh_id) return;
        s_csi_heard++;
        if (wslot == 0) s_slot_ref_us = now;   // slot 0's frame: the others time their slots from it
        mesh_note(info, wid);
        return;
    }
    if (s_csi_filter && memcmp(info->mac, s_csi_filter_mac, 6) != 0) return;
    s_csi_heard++;
    if (now < s_csi_next_us) { s_csi_skipped++; return; }
    // Due times advance by the period, so a faster transmitter averages out at `rate`; after a gap, start over.
    s_csi_next_us = (now - s_csi_next_us > s_csi_period_us) ? now + s_csi_period_us : s_csi_next_us + s_csi_period_us;
    csi_rec_t r;
    r.ms = (uint32_t)(now / 1000);
    r.rssi = (int8_t)info->rx_ctrl.rssi;
    r.nf = (int8_t)info->rx_ctrl.noise_floor;
    r.fmt = (uint8_t)info->rx_ctrl.cur_bb_format;
    r.id = ours ? wid : -1;
    r.nsc = (uint16_t)(info->len / 2);
    memcpy(r.mac, info->mac, 6);
    csi_bins(info, s_csi_k, r.amp);
    QueueHandle_t q = s_csi_queue;
    if (!q || xQueueSend(q, &r, 0) != pdTRUE) s_csi_dropped++;
}

// Stop and restart the Wi-Fi driver: with CSI (and, to transmit, the HT20 MCS0 raw-frame rate), or exactly as
// wifi_bring_up left it at boot (csi false, tx_ht false: WIFI_INIT_CONFIG_DEFAULT, STA, max TX power, both bands,
// every channel).
static esp_err_t csi_wifi_restart(bool csi, bool tx_ht) {
    s_wifi_restarting = true;
    (void)esp_wifi_set_promiscuous(false);
    (void)esp_wifi_stop();
    esp_err_t err = esp_wifi_deinit();
    if (err == ESP_ERR_WIFI_NOT_INIT) err = ESP_OK;
    if (err == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.csi_enable = csi ? 1 : 0;
        err = esp_wifi_init(&cfg);
    }
    if (err == ESP_OK) {
        (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK && tx_ht) {   // must come before esp_wifi_start
        wifi_tx_rate_config_t rc = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI };
        s_txrate_pre = esp_wifi_config_80211_tx(WIFI_IF_STA, &rc);
        ESP_LOGI(TAG, "csi: raw TX rate HT20 MCS0 before start: %s", esp_err_to_name(s_txrate_pre));
    }
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        (void)esp_wifi_set_max_tx_power(84);
#if SOC_WIFI_SUPPORT_5G
        if (s_has_5g) (void)esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
        open_all_channels();
    }
    s_wifi_restarting = false;
    ESP_LOGI(TAG, "csi: Wi-Fi restarted (csi %d, tx %d): %s, heap %lu", csi, tx_ht, esp_err_to_name(err),
             (unsigned long)esp_get_free_heap_size());
    return err;
}

static void csi_cfg_to(uint8_t sinks) {
    const csi_cfg_t c = s_csi_cfg;
    uint8_t self[6] = {0};
    esp_read_mac(self, ESP_MAC_WIFI_STA);   // the address tx frames carry, so a receiver can filter on it
    char mac[18] = "any";
    if (c.has_mac && c.mode == CSI_RX)
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);
    send_line_to(sinks, "WD:CSICFG mode=%s ch=%u mac=%s rate=%u sc=%u id=%u slot=%u slots=%u report=%u via=%s txrate=%s "
                 "self=%02X:%02X:%02X:%02X:%02X:%02X",
                 csi_mode_name(c.mode), (unsigned)c.ch, mac, (unsigned)c.rate,
                 (unsigned)(c.mode == CSI_RX ? c.sc : c.mode == CSI_MESH ? MESH_BINS : 0), (unsigned)c.id,
                 (unsigned)c.slot, (unsigned)c.slots, (unsigned)(c.mode == CSI_MESH ? c.report_ms : 0),
                 c.mode == CSI_RX ? "none" : c.raw ? "raw" : "espnow", c.mode == CSI_RX ? "none" : s_txrate,
                 self[0], self[1], self[2], self[3], self[4], self[5]);
}

/// The first time a transmitter shows up in a session (and again for a host that joins), name it.
static void csi_src_note(csi_src_t *srcs, int *n, uint16_t h, const uint8_t *mac, int id) {
    if (s_csi_resend_src) { s_csi_resend_src = false; *n = 0; }
    for (int i = 0; i < *n; i++) if (srcs[i].hash == h && !memcmp(srcs[i].mac, mac, 6)) return;
    if (*n >= CSI_SRC_MAX) *n = 0;
    srcs[*n].hash = h;
    memcpy(srcs[*n].mac, mac, 6);
    (*n)++;
    char idkv[16] = "";
    if (id >= 0) snprintf(idkv, sizeof(idkv), " id=%d", id);
    send_line("WD:CSISRC src=%04x mac=%02X:%02X:%02X:%02X:%02X:%02X%s", (unsigned)h, mac[0], mac[1], mac[2], mac[3],
              mac[4], mac[5], idkv);
}

static wifi_csi_config_t csi_acquire_config(void) {
    wifi_csi_config_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.enable = 1;
    cc.acquire_csi_legacy = 1;      // 11a/g OFDM: 5 GHz beacons, and 2.4 GHz APs without 11b rates
    cc.acquire_csi_ht20 = 1;        // HT data, and our own frames
    cc.acquire_csi_ht40 = 1;
    cc.acquire_csi_su = 1;          // HE SU: data from Wi-Fi 6 APs
#if CONFIG_SOC_WIFI_MAC_VERSION_NUM == 3   // ESP32-C5
    cc.acquire_csi_force_lltf = 0;
    cc.acquire_csi_vht = 1;
    cc.acquire_csi_he_stbc_mode = 2;
    cc.lltf_bit_mode = 1;           // L-LTF as 8-bit I/Q like every other estimate, so one parser fits all
#else                                      // ESP32-C6
    cc.acquire_csi_he_stbc = 2;
#endif
    return cc;
}

// TX timer (esp_timer task): send one frame, then arm the next. With slots, a board other than slot 0 sends at its
// slot's offset after slot 0's last frame (heard within MESH_REF_MS), else it free-runs at the period.
static void csi_tx_fire(void *arg) {
    (void)arg;
    if (!s_tx_on) return;
    uint32_t seq = s_tx_seq++;
    esp_err_t e;
    if (s_tx_raw) {
        s_tx_frame[41] = seq; s_tx_frame[42] = seq >> 8; s_tx_frame[43] = seq >> 16; s_tx_frame[44] = seq >> 24;
        e = esp_wifi_80211_tx(WIFI_IF_STA, s_tx_frame, sizeof(s_tx_frame), true);
    } else {
        s_tx_now[9] = seq; s_tx_now[10] = seq >> 8; s_tx_now[11] = seq >> 16; s_tx_now[12] = seq >> 24;
        e = esp_now_send(BCAST, s_tx_now, sizeof(s_tx_now));
    }
    if (e == ESP_OK) s_tx_sent++; else s_tx_fail++;
    const int64_t now = esp_timer_get_time(), p = s_tx_period_us;
    int64_t next, ref = s_slot_ref_us;
    if (s_tx_slots > 1 && s_tx_slot > 0 && ref && now - ref < (int64_t)MESH_REF_MS * 1000) {
        int64_t base = ref + (int64_t)s_tx_slot * p / s_tx_slots;
        next = base + ((now - base) / p + 1) * p;
    } else {
        next = s_tx_next_us + p;
        if (next < now) next = now + p;   // fell behind (the radio was busy): carry on, don't burst
    }
    if (next - now < 500) next += p;
    if (next - now > 2 * p) next = now + p;   // never wait longer than two periods (a torn 64-bit read of the reference)
    s_tx_next_us = next;
    if (s_tx_on) esp_timer_start_once(s_tx_timer, (uint64_t)(next - now));
}

/// The frame rate both transports must use for receivers to get CSI: HT20 MCS0, else legacy OFDM 6 Mbps. The driver's
/// default for raw frames and broadcast ESP-NOW is 1 Mbps DSSS, which carries no CSI at all (bench 2026-10-28: a C6
/// sending raw frames at the default gave a C5 receiver nothing). Records the outcome in s_txrate for WD:CSICFG.
static void csi_tx_rate(const csi_cfg_t *c) {
    wifi_tx_rate_config_t ht = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI };
    wifi_tx_rate_config_t ofdm = { .phymode = c->ch > 14 ? WIFI_PHY_MODE_11A : WIFI_PHY_MODE_11G, .rate = WIFI_PHY_RATE_6M };
    esp_err_t e_ht, e_ofdm = ESP_FAIL;
    if (c->raw) {
        e_ht = esp_wifi_config_80211_tx(WIFI_IF_STA, &ht);   // again after start: accepted either way, logged both ways
        if (e_ht != ESP_OK && s_txrate_pre == ESP_OK) e_ht = ESP_OK;
        if (e_ht != ESP_OK) {
            e_ofdm = esp_wifi_config_80211_tx(WIFI_IF_STA, &ofdm);
            if (e_ofdm != ESP_OK) e_ofdm = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_6M);
        }
    } else {
        e_ht = esp_now_set_peer_rate_config(BCAST, &ht);
        if (e_ht != ESP_OK) e_ofdm = esp_now_set_peer_rate_config(BCAST, &ofdm);
    }
    if (e_ht == ESP_OK) strlcpy(s_txrate, "ok", sizeof(s_txrate));
    else if (e_ofdm == ESP_OK) strlcpy(s_txrate, "ofdm6", sizeof(s_txrate));
    else strlcpy(s_txrate, esp_err_to_name(e_ht), sizeof(s_txrate));
    ESP_LOGI(TAG, "csi: %s TX rate: HT20 MCS0 %s, OFDM 6M %s -> txrate=%s", c->raw ? "raw" : "ESP-NOW",
             esp_err_to_name(e_ht), e_ht == ESP_OK ? "not needed" : esp_err_to_name(e_ofdm), s_txrate);
}

static esp_err_t csi_tx_start(const csi_cfg_t *c) {
    // via=raw: data frame, no DS bits, to the group address 03:57:44:43:<slot>:<id>; from and BSSID = this board. LLC/SNAP
    // with the IEEE local experimental EtherType 0x88B5, then "WDCSI", version 3, id, slot, slots, a 32-bit sequence
    // number. via=espnow (default): broadcast ESP-NOW (a vendor action frame, as Espressif's own CSI sender uses) carrying
    // the same "WDCSI" body. No beacon and no SSID either way: nothing shows up in a Wi-Fi list.
    s_tx_raw = c->raw;
    uint8_t *f = s_tx_frame;
    memset(f, 0, sizeof(s_tx_frame));
    f[0] = 0x08;
    memcpy(f + 4, CSI_GROUP, 4);
    f[8] = c->slot; f[9] = c->id;
    esp_read_mac(f + 10, ESP_MAC_WIFI_STA);
    memcpy(f + 16, f + 10, 6);
    static const uint8_t SNAP[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0xB5 };
    memcpy(f + 24, SNAP, 8);
    memcpy(f + 32, "WDCSI", 5);
    f[37] = 3; f[38] = c->id; f[39] = c->slot; f[40] = c->slots;
    memset(s_tx_now, 0, sizeof(s_tx_now));
    memcpy(s_tx_now, "WDCSI", 5);
    s_tx_now[5] = 3; s_tx_now[6] = c->id; s_tx_now[7] = c->slot; s_tx_now[8] = c->slots;
    if (!c->raw) {
        esp_err_t e = esp_now_init();
        if (e != ESP_OK) return e;
        s_espnow_on = true;
        esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_STA, .encrypt = false };   // channel 0: the one we're on
        memcpy(peer.peer_addr, BCAST, 6);
        if ((e = esp_now_add_peer(&peer)) != ESP_OK) return e;
    }
    csi_tx_rate(c);
    s_tx_seq = 0;
    s_tx_sent = s_tx_fail = 0;
    s_tx_period_us = 1000000 / c->rate;
    s_tx_slot = c->slot; s_tx_slots = c->slots;
    s_slot_ref_us = 0;
    s_tx_next_us = esp_timer_get_time();
    if (!s_tx_timer) {
        const esp_timer_create_args_t a = { .callback = csi_tx_fire, .name = "csi_tx" };
        esp_err_t e = esp_timer_create(&a, &s_tx_timer);
        if (e != ESP_OK) return e;
    }
    s_tx_on = true;
    return esp_timer_start_once(s_tx_timer, 1000);
}

static void csi_tx_stop(void) {
    s_tx_on = false;
    if (s_tx_timer) (void)esp_timer_stop(s_tx_timer);
    vTaskDelay(pdMS_TO_TICKS(5));   // a frame in flight in the timer callback finishes before Wi-Fi goes down
    if (s_espnow_on) { (void)esp_now_deinit(); s_espnow_on = false; }
}

/// Brings up the mode on the restarted driver. NULL on success, else the step that failed.
static const char *csi_setup(const csi_cfg_t *c, esp_err_t *err) {
    s_csi_mode = c->mode;
    s_mesh_id = c->id;
    if (c->mode != CSI_TX) {
        s_csi_k = c->sc;
        s_csi_filter = c->has_mac;
        memcpy(s_csi_filter_mac, c->mac, 6);
        s_csi_period_us = 1000000 / c->rate;
        s_csi_next_us = 0;
        s_csi_heard = s_csi_skipped = s_csi_dropped = 0;
        xQueueReset(s_csi_queue);
        wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA };
        (void)esp_wifi_set_promiscuous_filter(&filt);
        if ((*err = esp_wifi_set_promiscuous(true)) != ESP_OK) return "promiscuous";
    }
    if (c->mode != CSI_RX) {   // 20 MHz, so HT20 MCS0 frames go out as configured
#if SOC_WIFI_SUPPORT_5G
        wifi_bandwidths_t bw = { .ghz_2g = WIFI_BW_HT20, .ghz_5g = WIFI_BW_HT20 };
        esp_err_t e = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw);
#else
        esp_err_t e = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
#endif
        if (e != ESP_OK) ESP_LOGW(TAG, "csi: HT20 bandwidth not set (%s)", esp_err_to_name(e));
    }
    if ((*err = esp_wifi_set_channel(c->ch, WIFI_SECOND_CHAN_NONE)) != ESP_OK) return "channel";
    if (c->mode != CSI_TX) {
        wifi_csi_config_t cc = csi_acquire_config();
        if ((*err = esp_wifi_set_csi_config(&cc)) != ESP_OK) return "csi_config";
        if ((*err = esp_wifi_set_csi_rx_cb(csi_rx_cb, NULL)) != ESP_OK) return "csi_rx_cb";
        s_csi_rx_on = true;
        if ((*err = esp_wifi_set_csi(true)) != ESP_OK) return "csi_enable";
    }
    if (c->mode != CSI_RX && (*err = csi_tx_start(c)) != ESP_OK) return c->raw ? "tx_timer" : "espnow";
    return NULL;
}

static void csi_flush(char *line, int *len, int *batch) {
    if (!*batch) return;
    line[(*len)++] = '\n';
    send_chunks(line, (size_t)*len);
    *len = 0;
    *batch = 0;
}

static void csi_run_rx(const csi_cfg_t *c, char *line, csi_src_t *srcs) {
    csi_rec_t r;
    int len = 0, batch = 0, nsrc = 0;
    uint32_t batch_ms = 0, sent = 0, last_stat = csi_ms();
    uint32_t prev_heard = 0, prev_skipped = 0, prev_sent = 0;
    while (s_csi_running && host_linked()) {
        if (xQueueReceive(s_csi_queue, &r, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint16_t h = csi_src_hash(r.mac);
            csi_src_note(srcs, &nsrc, h, r.mac, r.id);
            if (!batch) {
                batch_ms = r.ms;
                len = snprintf(line, CSI_LINE_MAX, "WD:CSI ts=%lu ch=%u sc=%u p=", (unsigned long)r.ms, (unsigned)c->ch, (unsigned)c->sc);
            } else {
                line[len++] = ';';
            }
            len += snprintf(line + len, CSI_LINE_MAX - len, "%lu,%d,%d,%04x,%u,%u,", (unsigned long)(r.ms - batch_ms),
                            r.rssi, r.nf, (unsigned)h, (unsigned)r.fmt, (unsigned)r.nsc);
            for (int j = 0; j < c->sc; j++) {
                line[len++] = lower_hex_digit(r.amp[j] >> 4);
                line[len++] = lower_hex_digit(r.amp[j] & 0x0F);
            }
            batch++;
            sent++;
        }
        uint32_t now = csi_ms();
        if (batch && (batch >= CSI_BATCH_MAX || now - batch_ms >= CSI_BATCH_MS || len > CSI_LINE_MAX - CSI_PKT_MAX - 2))
            csi_flush(line, &len, &batch);
        if (now - last_stat >= 1000) {
            last_stat = now;
            uint32_t heard = s_csi_heard, skipped = s_csi_skipped;
            s_csi_pps = sent - prev_sent;
            send_line("WD:STATUS csi=1 mode=rx ch=%u pps=%lu heard=%lu skipped=%lu dropped=%lu", (unsigned)c->ch,
                      (unsigned long)s_csi_pps, (unsigned long)(heard - prev_heard), (unsigned long)(skipped - prev_skipped),
                      (unsigned long)s_csi_dropped);
            prev_heard = heard; prev_skipped = skipped; prev_sent = sent;
        }
    }
    s_csi_rx_on = false;
    (void)esp_wifi_set_csi(false);
    while (xQueueReceive(s_csi_queue, &r, 0) == pdTRUE) {}   // the last few, not worth a line after a stop
    csi_flush(line, &len, &batch);
}

static void csi_run_tx(const csi_cfg_t *c) {
    uint32_t prev_sent = 0, last_stat = csi_ms();
    while (s_csi_running && host_linked()) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t ms = csi_ms();
        if (ms - last_stat < 1000) continue;
        last_stat = ms;
        uint32_t sent = s_tx_sent;
        s_csi_pps = sent - prev_sent;
        prev_sent = sent;
        send_line("WD:STATUS csi=1 mode=tx ch=%u id=%u pps=%lu txfail=%lu", (unsigned)c->ch, (unsigned)c->id,
                  (unsigned long)s_csi_pps, (unsigned long)s_tx_fail);
    }
}

// Mesh: every report period, take every link's window and send them as one WD:CSIMESH line.
static void csi_run_mesh(const csi_cfg_t *c, char *line, mesh_link_t *snap) {
    uint32_t last_rep = csi_ms(), last_stat = last_rep, prev_sent = 0, prev_heard = 0, links_sec = 0;
    while (s_csi_running && host_linked()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t now = csi_ms();
        if (now - last_rep >= c->report_ms) {
            uint32_t win = now - last_rep;
            last_rep = now;
            portENTER_CRITICAL(&s_mesh_lock);
            memcpy(snap, s_mesh, MESH_LINKS * sizeof(mesh_link_t));
            for (int i = 0; i < MESH_LINKS; i++) {   // keep the id and mac, clear the window
                mesh_link_t *l = &s_mesh[i];
                l->n = 0; l->rssi_sum = 0; l->rssi_sq = 0;
                memset(l->a_sum, 0, sizeof(l->a_sum)); memset(l->a_sq, 0, sizeof(l->a_sq));
            }
            portEXIT_CRITICAL(&s_mesh_lock);
            int len = snprintf(line, CSI_LINE_MAX, "WD:CSIMESH ts=%lu id=%u ch=%u win=%lu l=", (unsigned long)now,
                               (unsigned)c->id, (unsigned)c->ch, (unsigned long)win);
            int links = 0;
            for (int i = 0; i < MESH_LINKS; i++) {
                const mesh_link_t *l = &snap[i];
                if (!l->used || !l->n) continue;
                const int64_t n = l->n, rs = l->rssi_sum;
                long rm = (long)(rs / n);   // RSSI mean (truncated toward 0) and spread in tenths of a dB
                int64_t rv100 = 100 * (n * (int64_t)l->rssi_sq - rs * rs) / (n * n);
                uint32_t rsd = isqrt32((uint32_t)(rv100 > 0 ? (rv100 > 0x7FFFFFFF ? 0x7FFFFFFF : rv100) : 0));
                uint32_t cv = 0;   // motion: mean over bins of (spread / mean), x1000
                char bins[2 * MESH_BINS + 1];
                for (int j = 0; j < MESH_BINS; j++) {
                    const int64_t as = l->a_sum[j];
                    uint32_t m = (uint32_t)(as / n);
                    int64_t var = (n * (int64_t)l->a_sq[j] - as * as) / (n * n);
                    uint32_t sd = isqrt32((uint32_t)(var > 0 ? var : 0));
                    if (m) cv += sd * 1000 / m;
                    uint32_t byte = m > 255 ? 255 : m;   // 128 = the packet mean
                    bins[2 * j] = lower_hex_digit(byte >> 4);
                    bins[2 * j + 1] = lower_hex_digit(byte & 0x0F);
                }
                bins[2 * MESH_BINS] = '\0';
                cv /= MESH_BINS;
                if (len > CSI_LINE_MAX - MESH_LINK_MAX - 2) break;
                len += snprintf(line + len, CSI_LINE_MAX - len, "%s%u,%lu,%ld,%lu,%lu,%s", links ? ";" : "", (unsigned)l->id,
                                (unsigned long)n, rm, (unsigned long)rsd, (unsigned long)cv, bins);
                links++;
            }
            links_sec = links > (int)links_sec ? (uint32_t)links : links_sec;
            if (links) { line[len++] = '\n'; send_chunks(line, (size_t)len); }
        }
        if (now - last_stat >= 1000) {
            last_stat = now;
            uint32_t sent = s_tx_sent, heard = s_csi_heard;
            s_csi_pps = links_sec;
            send_line("WD:STATUS csi=1 mode=mesh ch=%u id=%u links=%lu heard=%lu pps=%lu txfail=%lu synced=%d dropped=%lu",
                      (unsigned)c->ch, (unsigned)c->id, (unsigned long)links_sec, (unsigned long)(heard - prev_heard),
                      (unsigned long)(sent - prev_sent), (unsigned long)s_tx_fail,
                      c->slots > 1 ? (c->slot == 0 || (s_slot_ref_us && esp_timer_get_time() - s_slot_ref_us < (int64_t)MESH_REF_MS * 1000)) : 0,
                      (unsigned long)s_csi_dropped);
            prev_sent = sent; prev_heard = heard; links_sec = 0;
        }
    }
    s_csi_rx_on = false;
    (void)esp_wifi_set_csi(false);
}

static void csi_task(void *arg) {
    (void)arg;
    s_crumb = CR_CSI;
    const csi_cfg_t c = s_csi_cfg;   // this session's settings; a new `csi` command restarts the task
    s_crumb_ch = c.ch;
    s_csi_pps = 0;
    send_line("WD:MODE mode=csi");
    send_line("WD:BEGIN type=csi interval=0 channel=%u", (unsigned)c.ch);
    char *line = c.mode != CSI_TX ? malloc(CSI_LINE_MAX) : NULL;
    csi_src_t *srcs = c.mode == CSI_RX ? calloc(CSI_SRC_MAX, sizeof(csi_src_t)) : NULL;
    mesh_link_t *snap = c.mode == CSI_MESH ? malloc(MESH_LINKS * sizeof(mesh_link_t)) : NULL;
    if (c.mode == CSI_MESH) s_mesh = calloc(MESH_LINKS, sizeof(mesh_link_t));
    esp_err_t err = ESP_OK;
    const char *fail = NULL;
    if ((c.mode != CSI_TX && !line) || (c.mode == CSI_RX && !srcs) || (c.mode == CSI_MESH && (!snap || !s_mesh))) {
        fail = "memory"; err = ESP_ERR_NO_MEM;
    }
    strlcpy(s_txrate, "none", sizeof(s_txrate));
    s_txrate_pre = ESP_ERR_NOT_FINISHED;
    if (!fail && (err = csi_wifi_restart(c.mode != CSI_TX, c.mode != CSI_RX && c.raw)) != ESP_OK) fail = "wifi_restart";
    if (!fail) fail = csi_setup(&c, &err);
    if (fail) {
        send_line("WD:ERR reason=csi_failed step=%s err=%s", fail, esp_err_to_name(err));
        ESP_LOGW(TAG, "csi: %s failed: %s", fail, esp_err_to_name(err));
        // Back to what the board was doing; the restore below brings Wi-Fi back first.
        if (s_role == ROLE_CSI && s_csi_running) {
            s_role = s_csi_prev == ROLE_CSI ? ROLE_WIFI : s_csi_prev;
            s_switch_pending = true;
            send_line("WD:MODE mode=%s", role_name(s_role));
        }
    } else {
        csi_cfg_to(SINK_ALL);
        ESP_LOGI(TAG, "csi %s on channel %u", csi_mode_name(c.mode), (unsigned)c.ch);
        if (c.mode == CSI_TX) csi_run_tx(&c);
        else if (c.mode == CSI_MESH) csi_run_mesh(&c, line, snap);
        else csi_run_rx(&c, line, srcs);
    }
    s_csi_rx_on = false;
    csi_tx_stop();
    err = csi_wifi_restart(false, false);   // exactly as booted; no CSI callback can run after this
    if (err != ESP_OK) {
        send_line("WD:ERR reason=csi_wifi_restore err=%s", esp_err_to_name(err));
        ESP_LOGE(TAG, "csi: Wi-Fi didn't come back (%s), rebooting", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    mesh_link_t *m = s_mesh;
    s_mesh = NULL;
    free(m);
    free(snap);
    free(line);
    free(srcs);
    send_line("WD:END reason=stop");
    s_csi_pps = 0;
    s_crumb = CR_IDLE;
    s_csi_running = false;
    s_csi_task = NULL;
    if (s_switch_pending) { s_switch_pending = false; stream_start(); }
    vTaskDelete(NULL);
}

static void csi_start(void) {
    if (s_csi_running || s_csi_task || s_ota_active || !host_linked()) return;
    if (!s_csi_queue) s_csi_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_rec_t));
    if (!s_csi_queue) { send_line("WD:ERR reason=no_memory"); return; }
    s_csi_running = true;
    if (xTaskCreate(csi_task, "wd_csi", 6144, NULL, 5, &s_csi_task) != pdPASS) {
        s_csi_running = false;
        s_csi_task = NULL;
        send_line("WD:ERR reason=no_task");
        return;
    }
    ESP_LOGI(TAG, "csi started");
}
#else
static void csi_start(void) {}
#endif

static volatile bool s_rebooting = false;

/// Starts scanning in this board's current role.
static void stream_start(void) {
    if (s_rebooting) return;
    switch (s_role) {
    case ROLE_BT:  bt_start();  break;
    case ROLE_CAP: cap_start(); break;
    case ROLE_CSI: csi_start(); break;
    default:       wifi_start(); break;
    }
}

static void stream_stop(void) {
    s_streaming = false;
    s_bt_running = false;
    s_cap_running = false;
    s_csi_running = false;
    // The CSI role restarts the driver itself and never scans: leave it alone while that task exists.
    if (!s_wifi_restarting && !s_csi_task) (void)esp_wifi_scan_stop();
}

/// Waits for the scan, Bluetooth, capture and CSI tasks to send what they hold and exit, then for their stacks to be freed.
static void wait_role_tasks(uint32_t max_ms) {
    for (uint32_t t = 0; t < max_ms && (s_scan_task || s_bt_task || s_cap_task || s_csi_task); t += 20) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(50));
}

/// Switches the board between Wi-Fi, Bluetooth, Capture and CSI roles. A running task finishes first; the task that
/// ends then starts the new role.
static const char *role_name(scan_role_t role) {
    return role == ROLE_BT ? "bt" : role == ROLE_CAP ? "cap" : role == ROLE_CSI ? "csi" : "wifi";
}

static void set_role(scan_role_t role) {
    const char *name = role_name(role);
    if (role == s_role) {
        send_line("WD:MODE mode=%s", name);
        if (!s_streaming && !s_bt_running && !s_cap_running && !s_csi_running &&
            !s_scan_task && !s_bt_task && !s_cap_task && !s_csi_task) stream_start();
        return;
    }
    s_role = role;
    bt_power_kick();
    send_line("WD:MODE mode=%s", name);
    if (s_scan_task || s_bt_task || s_cap_task || s_csi_task) {
        s_switch_pending = true;
        stream_stop();
    } else {
        stream_start();
    }
}

/// A host linked (BLE subscribe or USB hello): start scanning, or bring it up to date with the scan already running.
static void host_joined(uint8_t sink) {
    heap_check("host joined");
    if (s_streaming || s_bt_running || s_cap_running || s_csi_running) {
        const char *m = role_name(s_role);
        send_header(sink);
        send_line_to(sink, "WD:MODE mode=%s", m);
        if (s_role == ROLE_CSI) {   // the CSI role sits on one channel and names its transmitters again for this host
#if CSI_SUPPORTED
            send_line_to(sink, "WD:BEGIN type=csi interval=0 channel=%u", (unsigned)s_csi_cfg.ch);
            csi_cfg_to(sink);
            s_csi_resend_src = true;
#endif
            return;
        }
        send_line_to(sink, "WD:BEGIN type=%s interval=0 channel=auto", m);
        if (s_role != ROLE_BT) send_plan_to(sink);
        return;
    }
    if (s_role != ROLE_WIFI || s_ota_active) send_header(sink);   // only the Wi-Fi scan task sends the header itself
    if (s_scan_task || s_bt_task || s_cap_task || s_csi_task) s_switch_pending = true;   // still finishing: it restarts the role
    else stream_start();
}

// ─── Firmware update over BLE and USB (PROTOCOL.md "Firmware update") ──────
// Image bytes arrive on the OTA characteristic and are queued; this task writes them to the spare app slot
// (sequential writes erase sector by sector, so nothing blocks the BLE host for long) and acks progress.
// The receive buffer is created once and only reset, never freed, so a late BLE write can't land in freed memory.
// It is created after the scan tasks have exited, at the largest size the heap allows.
static void usb_arm_raw(uint8_t sink, uint32_t size);

/// The capture and CSI frame queues stay after their task ends (the capture one is 8 KB); give them back when the
/// heap is needed for something else (a firmware update, an offline run).
static void free_idle_queues(void) {
    if (s_cap_queue && !s_cap_task && !s_cap_running) {   // recreated by the next capture start
        QueueHandle_t q = s_cap_queue;
        s_cap_queue = NULL;
        vQueueDelete(q);
    }
#if CSI_SUPPORTED
    if (s_csi_queue && !s_csi_task) {   // the driver was restarted without CSI, so nothing can still write to it
        QueueHandle_t q = s_csi_queue;
        s_csi_queue = NULL;
        vQueueDelete(q);
    }
#endif
}

static const char *ota_prepare(void) {
    wait_role_tasks(2000);
    if (s_ota_abort_req) return "aborted";
    free_idle_queues();
    if (!s_ota_buf) {
        for (size_t i = 0; i < sizeof(OTA_BUF_SIZES) / sizeof(OTA_BUF_SIZES[0]) && !s_ota_buf; i++) {
            s_ota_buf = xStreamBufferCreate(OTA_BUF_SIZES[i], 1);
            if (s_ota_buf) s_ota_buf_bytes = OTA_BUF_SIZES[i];
        }
        if (!s_ota_buf) return "no_memory";
    } else {
        xStreamBufferReset(s_ota_buf);
    }
    uint32_t w = (uint32_t)(s_ota_buf_bytes - OTA_BUF_SLACK) / OTA_ACK_STEP * OTA_ACK_STEP;
    if (w > OTA_WINDOW_MAX) w = OTA_WINDOW_MAX;
    if (w < OTA_ACK_STEP) w = OTA_ACK_STEP;
    s_ota_window = w;
    s_ota_ack_step = w >= 2 * OTA_ACK_STEP ? OTA_ACK_STEP : w / 2;   // keep an ack due inside every window
    if (esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle) != ESP_OK) return "begin_failed";
    return NULL;
}

static void ota_cleanup(void) {
    bool was_ble = s_ota_active && s_ota_src == SINK_BLE;
    s_ota_active = false;
    s_ota_end_req = false;
    s_ota_abort_req = false;
    s_ota_task = NULL;
    if (was_ble) request_link_params(false);   // back to the normal link timing
}

static void ota_task(void *arg) {
    (void)arg;
    static uint8_t chunk[1024];
    const char *err = ota_prepare();
    if (err) {
        send_line_to(s_ota_src, "WD:OTA error=%s", err);
        ESP_LOGW(TAG, "update not started: %s", err);
        ota_cleanup();
        vTaskDelete(NULL);
        return;
    }
    if (s_ota_src != SINK_BLE) usb_arm_raw(s_ota_src, s_ota_size);
    else request_link_params(true);   // faster link timing while the image transfers
    ESP_LOGI(TAG, "update started: %lu bytes → %s, buffer %u", (unsigned long)s_ota_size, s_ota_part->label,
             (unsigned)s_ota_buf_bytes);
    send_line_to(s_ota_src, "WD:OTA ready window=%u size=%u", (unsigned)s_ota_window, (unsigned)s_ota_buf_bytes);

    uint32_t written = 0, next_ack = s_ota_ack_step;
    s_ota_written = 0;
    bool ended = false;
    for (;;) {
        if (s_ota_abort_req) { err = "aborted"; break; }
        size_t n = xStreamBufferReceive(s_ota_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
        if (n > 0) {
            if (written + n > s_ota_size) { err = "too_much_data"; break; }
            s_crumb = CR_OTA;
            if (esp_ota_write(s_ota_handle, chunk, n) != ESP_OK) { err = "write_failed"; break; }
            written += n;
            s_ota_written = written;
            if (written >= next_ack || written == s_ota_size) {
                send_line_to(s_ota_src, "WD:OTA ack=%lu", (unsigned long)written);
                next_ack = written + s_ota_ack_step;
            }
        }
        if (s_ota_end_req && xStreamBufferIsEmpty(s_ota_buf)) {
            if (written != s_ota_size) { err = "short_image"; break; }
            ended = true;   // esp_ota_end releases the handle whatever it returns
            if (esp_ota_end(s_ota_handle) != ESP_OK) { err = "invalid_image"; break; }
            if (esp_ota_set_boot_partition(s_ota_part) != ESP_OK) { err = "set_boot_failed"; break; }
            send_line_to(s_ota_src, "WD:OTA done");
            ESP_LOGI(TAG, "update written (%lu bytes) — restarting", (unsigned long)written);
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        }
    }
    if (!ended) esp_ota_abort(s_ota_handle);
    send_line_to(s_ota_src, "WD:OTA error=%s", err);
    ESP_LOGW(TAG, "update failed: %s", err);
    ota_cleanup();
    vTaskDelete(NULL);
}

// Runs in the command context, so it only validates and stops scanning; ota_task waits for the scan tasks to send
// what they hold, then sets up the update and replies WD:OTA ready.
static void ota_begin(const char *args) {
    const uint8_t src = s_cmd_src;
    if (s_ota_active) { send_line_to(src, "WD:OTA error=busy"); return; }
    unsigned long size = strtoul(args, NULL, 10);
    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) { send_line_to(src, "WD:OTA error=no_update_slot"); return; }
    if (size == 0 || size > s_ota_part->size) { send_line_to(src, "WD:OTA error=bad_size"); return; }
    s_ota_size = (uint32_t)size;
    s_ota_written = 0;
    s_ota_src = src;
    s_ota_end_req = false;
    s_ota_abort_req = false;
    s_ota_active = true;   // keeps the scan roles from restarting
    s_switch_pending = false;
    stream_stop();
    if (xTaskCreate(ota_task, "ota", 6144, NULL, 4, &s_ota_task) != pdPASS) {
        s_ota_active = false;
        s_ota_task = NULL;
        send_line_to(src, "WD:OTA error=no_task");
    }
}

static int on_ota_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (!s_ota_active || !s_ota_buf || s_ota_src != SINK_BLE) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    uint8_t buf[512];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) return BLE_ATT_ERR_UNLIKELY;
    if (xStreamBufferSend(s_ota_buf, buf, got, 0) != got) s_ota_abort_req = true;   // phone ignored the window
    return 0;
}

static void ota_mark_valid_if_pending(void) {
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

// ─── Boot diagnostics ──────────────────────────────────────────────────────
// Each boot saves how far it got (NVS, shared by both app slots), so after a hang, crash or rollback the next
// boot can report it. Freshly updated firmware that isn't up within BOOT_WATCHDOG_S restarts itself, and the
// bootloader then rolls back to the previous slot.
enum { STAGE_NONE = 0, STAGE_STORAGE = 1, STAGE_WIFI = 3, STAGE_BLUETOOTH, STAGE_READY, STAGE_WATCHDOG };   // 2 was the LED
#define BOOT_WATCHDOG_S 30

static esp_timer_handle_t s_boot_wd;

static const char *reset_reason_key(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:   return "power_on";
    case ESP_RST_EXT:       return "reset_pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    default:                return "other";
    }
}

static const char *stage_name(uint8_t s) {
    switch (s) {
    case STAGE_STORAGE:   return "storage";
    case STAGE_WIFI:      return "wifi";
    case STAGE_BLUETOOTH: return "bluetooth";
    case STAGE_READY:     return "ready";
    case STAGE_WATCHDOG:  return "boot_watchdog";
    default:              return "none";
    }
}

static const char *image_state_name(esp_ota_img_states_t s) {
    switch (s) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "trial";
    case ESP_OTA_IMG_VALID:          return "confirmed";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static void boot_stage(uint8_t stage) {
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "stage", stage);
    nvs_set_str(h, "stfw", FW_VERSION);
    nvs_commit(h);
    nvs_close(h);
}

static void boot_watchdog_fire(void *arg) {
    (void)arg;
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        boot_stage(STAGE_WATCHDOG);
        esp_restart();   // still on trial → the bootloader rolls back to the previous firmware
    }
}

static const char *crumb_name(uint8_t c) {
    switch (c) {
    case CR_SCAN:    return "wifi_scan";
    case CR_RESULTS: return "wifi_results";
    case CR_SEND_AP: return "sending_aps";
    case CR_BT:      return "bt_scan";
    case CR_CAP:     return "capture";
    case CR_OTA:     return "ota_write";
    case CR_CMD:     return "command";
    case CR_GPS:     return "gps";
    case CR_CSI:     return "csi";
    default:         return "idle";
    }
}

/// WD:CRASH from the record the panic handler left in RTC memory (lost on power loss, kept across a crash reset).
static void crash_report_build(void) {
    esp_reset_reason_t r = esp_reset_reason();
    bool bad = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (bad && s_crash.magic == CRASH_MAGIC) {
        char reason[sizeof(s_crash.reason)];
        size_t i = 0;
        for (; i + 1 < sizeof(reason) && s_crash.reason[i]; i++) {
            char c = s_crash.reason[i];
            reason[i] = (c == ' ' || c == '=') ? '_' : (c < 32 || c > 126) ? '?' : c;
        }
        reason[i] = '\0';
        s_crash.task[sizeof(s_crash.task) - 1] = '\0';
        snprintf(s_crash_line, sizeof(s_crash_line),
                 "WD:CRASH kind=%s reason=%s task=%s pc=0x%08lx core=%u up_ms=%lu last=%s ch=%u heap_min=%lu",
                 s_crash.kind == CK_STACK ? "stack_overflow" : "panic", reason[0] ? reason : "?",
                 s_crash.task[0] ? s_crash.task : "?", (unsigned long)s_crash.pc, (unsigned)s_crash.core,
                 (unsigned long)s_crash.up_ms, crumb_name(s_crash.crumb), (unsigned)s_crash.ch,
                 (unsigned long)s_crash.heap_min);
    } else if (bad) {
        snprintf(s_crash_line, sizeof(s_crash_line), "WD:CRASH kind=%s reason=unrecorded",
                 r == ESP_RST_PANIC ? "panic" : "watchdog");
    }
    if (s_crash_line[0]) ESP_LOGW(TAG, "%s", s_crash_line);
    s_crash.magic = 0;
}

/// Call right after NVS is up: reads the previous boot's progress, counts this boot, builds the WD:BOOT line,
/// and arms the boot watchdog when this firmware is on trial after an update.
static void boot_diag_begin(void) {
    uint8_t prev_stage = STAGE_NONE;
    char prev_fw[16] = "?";
    uint32_t boots = 0;
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(prev_fw);
        nvs_get_u8(h, "stage", &prev_stage);
        if (nvs_get_str(h, "stfw", prev_fw, &len) != ESP_OK) strcpy(prev_fw, "?");
        nvs_get_u32(h, "boots", &boots);
        boots++;
        nvs_set_u32(h, "boots", boots);
        nvs_commit(h);
        nvs_close(h);
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    snprintf(s_boot_line, sizeof(s_boot_line),
             "WD:BOOT fw=%s slot=%s image=%s reset=%s boots=%lu prev_fw=%s prev_stage=%s rolled_back=%s",
             FW_VERSION, run ? run->label : "?", image_state_name(st),
             reset_reason_key(esp_reset_reason()), (unsigned long)boots, prev_fw, stage_name(prev_stage),
             bad ? bad->label : "none");
    ESP_LOGI(TAG, "%s", s_boot_line);
    crash_report_build();
    boot_stage(STAGE_STORAGE);
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_timer_create_args_t args = { .callback = boot_watchdog_fire, .name = "boot_wd" };
        if (esp_timer_create(&args, &s_boot_wd) == ESP_OK)
            esp_timer_start_once(s_boot_wd, (uint64_t)BOOT_WATCHDOG_S * 1000000ULL);
    }
}

// ─── Terminal commands (PROTOCOL.md "Commands") ────────────────────────────
static bool apply_channel_list(const char *list) {
    uint8_t chans[MAX_CHANNELS];
    size_t count = parse_channel_list(list, chans);
    if (count == 0) return false;
    portENTER_CRITICAL(&s_plan_lock);
    memcpy(s_plan, chans, count);
    s_plan_n = count;
    portEXIT_CRITICAL(&s_plan_lock);
    if (s_streaming) send_plan_line();
    return true;
}

static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (low power)";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "other";
    }
}

// ─── Board identity: LED rings and battery gauge ──────────────────────────
// WS2812 pixels are write-only, so an LED pin is only ever driven for a board identified by a hardware fingerprint
// (board_detect). Without one, no LED pin is touched. Hardware facts: docs/device-research/README.md.
typedef enum { BOARD_NONE = 0, BOARD_CRUMB, BOARD_GHOSTBOARD, BOARD_POLTERGEIST,
               BOARD_BISCUIT_PRO, BOARD_BISCUIT_ULTRA, BOARD_BISCUIT_DUAL, BOARD_KOKO_V8,
               BOARD_MINI_V3, BOARD_AWOK_DUAL_C5, BOARD_WAVESHARE_AMOLED_206 } board_t;
typedef struct { const char *id, *name; int led_gpio, pixels; } board_def_t;
static const board_def_t BOARDS[] = {
    [BOARD_NONE]          = { NULL, "generic", -1, 0 },
    [BOARD_CRUMB]         = { "crumb", "Biscuit Crumb", 23, 6 },
    [BOARD_GHOSTBOARD]    = { "ghostboard", "Rabbit Labs Ghost Board", 10, 3 },
    [BOARD_POLTERGEIST]   = { "poltergeist", "Rabbit Labs Poltergeist", 27, 1 },
    // Two-chip Biscuits, identified by the WardriveESP co-processor on their WROOM (WDCP hello, see coproc_line).
    // Both stock C5 images drive one NeoPixel on GPIO 10; the Pro has it fitted, the bench Ultra shows none, so
    // the Ultra gets no ring. biscuit_dual = the co-processor hasn't been told which model it is.
    [BOARD_BISCUIT_PRO]   = { "biscuit_pro", "Biscuit Pro", 10, 1 },
    [BOARD_BISCUIT_ULTRA] = { "biscuit_ultra", "Biscuit Ultra", 10, 0 },
    [BOARD_BISCUIT_DUAL]  = { "biscuit_dual", "Biscuit Pro or Ultra", 10, 1 },
    // Same MAX17048 on 5/4 as the Crumb, told apart by its ILI9341 screen (lcd_probe_koko). No ring.
    [BOARD_KOKO_V8]       = { "koko_v8", "Koko Marauder V8", -1, 0 },
    // No gauge; told apart by its ST7735 screen answering its ID read (lcd_probe_mini). No ring.
    [BOARD_MINI_V3]       = { "marauder_mini_v3", "Marauder Mini v3", -1, 0 },
    // The screen chip of an AWOK Dual C5 (its white port; the orange port's C5 has no screen and stays generic): the
    // Koko V8's ILI9341 layout on CS 23 / DC 24, but no battery gauge on 5/4.
    [BOARD_AWOK_DUAL_C5]  = { "awok_dual_c5", "AWOK Dual C5", -1, 0 },
    // ESP32-S3 watch: AXP2101 power chip (IC type 0x4A) and its other board-bus parts on SDA 15 / SCL 14 (watch_probe),
    // 410x502 QSPI AMOLED.
    // The name fills s_board_caps exactly (63 characters with the id); keep it this short.
    [BOARD_WAVESHARE_AMOLED_206] = { "waveshare_s3_amoled_206", "Waveshare AMOLED 2.06", -1, 0 },
};
static board_t s_board = BOARD_NONE;
static bool board_is_awok(void) { return s_board == BOARD_AWOK_DUAL_C5; }
static bool board_gps14(void) {   // GPS on GPIO 14 (GhostESP / Marauder configs: GPS_UART_RX_PIN 14)
    return s_board == BOARD_AWOK_DUAL_C5 || s_board == BOARD_MINI_V3 || s_board == BOARD_KOKO_V8;
}
static volatile int s_batt_pct = -1;    // state of charge, -1 = unknown
static volatile int s_batt_mv = 0;

#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
#define LED_MAX 6

// ── LED ring: 800 kHz WS2812 (40 MHz ticks: one = 28 high / 20 low, zero = 10 / 38), GRB ──
// One pattern per kind of app alert; the app picks the pattern and passes the banner's category color.
typedef enum { LED_IDLE = 0, LED_POLICE, LED_HEARTBEAT, LED_STROBE, LED_SPIN, LED_PULSE, LED_SPARKLE, LED_STORM, LED_TEST } led_fx_t;
#define LED_FRAME_MS 16   // ~60 fps: smooth enough for the dim end of the idle breath, still a tiny task
static int s_led_gpio = -1;
static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_enc;
static TaskHandle_t s_led_task;
static volatile led_fx_t s_led_fx = LED_IDLE;
static volatile uint32_t s_led_rgb, s_led_since, s_led_until;   // alert color 0xRRGGBB, start, end (ms)
static volatile uint8_t  s_led_bright = 40;                     // percent
static volatile bool     s_led_stealth = false;                 // ring fully dark
static volatile bool     s_led_paused = false;                  // pinsense has the pin
static volatile bool     s_led_quit = false;                    // led_stop: blank the ring and end the task

static uint32_t led_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void led_show(uint8_t px[LED_MAX][3]) {
    static uint8_t grb[LED_MAX * 3];   // the RMT driver keeps reading this after rmt_transmit returns: never a stack buffer
    for (int i = 0; i < s_led_n; i++) { grb[i * 3] = px[i][1]; grb[i * 3 + 1] = px[i][0]; grb[i * 3 + 2] = px[i][2]; }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(s_led_chan, s_led_enc, grb, (size_t)s_led_n * 3, &tc) == ESP_OK) rmt_tx_wait_all_done(s_led_chan, -1);
}

static void led_set(float p[3], uint32_t rgb, float f) {   // 0-255 per channel, before dithering
    p[0] = ((rgb >> 16) & 0xFF) * f; p[1] = ((rgb >> 8) & 0xFF) * f; p[2] = (rgb & 0xFF) * f;
}

// Battery color: red at empty → amber at 35% → green from 70%. Cyan without a gauge (or before it answered).
static uint32_t batt_rgb(void) {
    int p = s_has_batt ? s_batt_pct : -1;
    if (p < 0) return 0x00A0FF;
    if (p >= 70) return 0x00FF00;
    if (p >= 35) { int t = (p - 35) * 255 / 35; return (uint32_t)((255 - t) << 16) | (uint32_t)((160 + 95 * t / 255) << 8); }
    int t = p * 255 / 35;
    return 0xFF0000 | (uint32_t)((160 * t / 255) << 8);
}

static void hue_rgb(float h, float out[3]) {   // h in degrees, full saturation and value
    float x = fmodf(h, 360.0f) / 60.0f, f = x - floorf(x), q = 1.0f - f;
    switch ((int)x) {
    case 0:  out[0] = 1; out[1] = f; out[2] = 0; break;
    case 1:  out[0] = q; out[1] = 1; out[2] = 0; break;
    case 2:  out[0] = 0; out[1] = 1; out[2] = f; break;
    case 3:  out[0] = 0; out[1] = q; out[2] = 1; break;
    case 4:  out[0] = f; out[1] = 0; out[2] = 1; break;
    default: out[0] = 1; out[1] = 0; out[2] = q; break;
    }
}

#define BREATH_MS 4500

static void led_task(void *arg) {
    (void)arg;
    float px[LED_MAX][3], err[LED_MAX][3] = {{0}}, spark[LED_MAX] = {0};
    uint8_t out[LED_MAX][3];
    const int n = s_led_n;
    bool dark = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LED_FRAME_MS));
        if (s_led_quit) {   // led_stop: leave the pixels dark, then end; led_stop frees the channel
            if (!s_led_paused) { memset(out, 0, sizeof(out)); led_show(out); }
            s_led_quit = false;
            vTaskDelete(NULL);
        }
        if (s_led_paused) continue;
        uint32_t now = led_ms();
        if (now < 8000 || now % 2000 < LED_FRAME_MS) heap_check("led frame");
        led_fx_t fx = s_led_fx;
        if (fx != LED_IDLE && (int32_t)(now - s_led_until) >= 0) s_led_fx = fx = LED_IDLE;
        if (s_led_stealth) {
            if (!dark) { memset(out, 0, sizeof(out)); led_show(out); dark = true; }
            continue;
        }
        dark = false;
        uint32_t t = now - s_led_since, rgb = s_led_rgb;
        float scale = s_led_bright / 100.0f;

        // Idle, and the base the storm fades back into: a cosine-eased breath in the battery's color, stepped evenly to
        // the eye (gamma 2.2) and kept in float until dithering.
        float b = 0.5f - 0.5f * cosf(6.2831853f * (float)(now % BREATH_MS) / BREATH_MS);
        float level = powf(0.23f + 0.30f * b, 2.2f) * scale;
        for (int i = 0; i < n; i++) led_set(px[i], batt_rgb(), level);

        switch (fx) {
        case LED_STORM: {    // a phone or USB host just linked: a fast rainbow swirl with sparkle that fades back to idle
            uint32_t dur = s_led_until - s_led_since;
            float env = t < 120 ? t / 120.0f : 1.0f - (float)(t - 120) / (float)(dur > 120 ? dur - 120 : 1);
            if (env < 0) env = 0;
            float surge = scale * 1.6f > 1.0f ? 1.0f : scale * 1.6f;
            for (int i = 0; i < n; i++) {
                float c[3];
                hue_rgb((float)t * 0.9f + i * 360.0f / n, c);   // a full turn of hue every ~400 ms
                spark[i] *= 0.82f;
                if ((esp_random() & 15) == 0) spark[i] = 1.0f;
                for (int j = 0; j < 3; j++) {
                    float v = (c[j] * 0.85f + spark[i] * 0.6f) * 255.0f * surge;
                    px[i][j] = px[i][j] * (1 - env) + (v > 255 ? 255 : v) * env;
                }
            }
            break;
        }
        case LED_POLICE: {   // red/blue strobe: halves chasing around a full ring, the whole ring swapping on a small one
            int off = (int)(t / 110) % n;
            float v = ((t / 55) % 2 ? 1.0f : 0.27f) * scale;
            for (int i = 0; i < n; i++) {
                bool red = n >= 6 ? (i + off) % n < n / 2 : (t / 110) % 2 == 0;
                led_set(px[i], red ? 0xFF0000 : 0x0000FF, v);
            }
            break;
        }
        case LED_SPIN: {     // a comet circling in the color: trackers, Find My, drones, Remote ID
            static const float TAIL[3] = { 1.0f, 0.35f, 0.1f };
            int head = (int)(t / 90) % n;
            memset(px, 0, sizeof(px));
            for (int k = 0; k < 3 && k < n; k++) led_set(px[(head - k + n) % n], rgb, TAIL[k] * scale);
            break;
        }
        case LED_PULSE: {    // the whole ring breathing fast in the color: cameras
            float e = 0.5f - 0.5f * cosf(6.2831853f * (float)(t % 900) / 900.0f);
            for (int i = 0; i < n; i++) led_set(px[i], rgb, (0.12f + 0.88f * e) * scale);
            break;
        }
        case LED_HEARTBEAT: { // lub-dub, then rest: adult toys, health devices
            uint32_t ph = t % 1100;
            float e = ph < 140 ? sinf(3.1415927f * ph / 140.0f) : (ph >= 260 && ph < 400) ? 0.75f * sinf(3.1415927f * (ph - 260) / 140.0f) : 0.0f;
            for (int i = 0; i < n; i++) led_set(px[i], rgb, (0.06f + 0.94f * e) * scale);
            break;
        }
        case LED_STROBE: {   // hard flashes, halves swapping: attack tools, gunshot sensors
            bool on = (t / 45) % 2 == 0;
            int half = (int)(t / 180) % 2;
            memset(px, 0, sizeof(px));
            if (on) for (int i = 0; i < n; i++) if (n < 4 || (i < n / 2) == (half == 0)) led_set(px[i], rgb, scale);
            break;
        }
        case LED_SPARKLE:    // pixels flare and fade in the color: captures (gold), cracked passwords (green)
            for (int i = 0; i < n; i++) {
                spark[i] *= 0.88f;
                if ((esp_random() & 15) == 0) spark[i] = 1.0f;
                led_set(px[i], rgb, spark[i] * scale);
            }
            break;
        case LED_TEST: {     // red, green, blue, white, each filling the ring pixel by pixel
            static const uint32_t C[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
            int lit = (int)((t % 1000) * n / 1000);
            memset(px, 0, sizeof(px));
            for (int i = 0; i <= lit && i < n; i++) led_set(px[i], C[(t / 1000) % 4], scale);
            break;
        }
        default:
            break;
        }

        // Temporal dithering: each channel carries its leftover fraction into the next frame, so a level between two
        // 8-bit steps averages out at 60 fps instead of stepping visibly at the dim end.
        for (int i = 0; i < n; i++) for (int j = 0; j < 3; j++) {
            float v = px[i][j] + err[i][j];
            int q = (int)v;
            if (q < 0) q = 0; else if (q > 255) q = 255;
            err[i][j] = v - (float)q;
            if (err[i][j] > 1.0f || err[i][j] < -1.0f) err[i][j] = 0;   // clipped: don't let it wind up
            out[i][j] = (uint8_t)q;
        }
        led_show(out);
    }
}

/// A phone or USB host linked: a short rainbow storm, unless an alert holds the ring.
static void led_storm(void) {
    lcd_alert("LINKED", 1500, true, 0x00E5FF, false);   // the dashboard's Bluetooth cyan
    if (!s_led_task || s_led_fx != LED_IDLE) return;
    uint32_t now = led_ms();
    s_led_since = now; s_led_until = now + 1500;
    s_led_fx = LED_STORM;
    display_alert("LINKED", 1500);
}

static void led_save(void) {
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ledbright", s_led_bright);
    nvs_set_u8(h, "ledstealth", s_led_stealth ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static rmt_tx_channel_config_t led_channel_config(int gpio) {
    rmt_tx_channel_config_t cc = { .gpio_num = gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 40000000,
                                   .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL, .trans_queue_depth = 2 };
    return cc;
}

static void led_start(int gpio, int pixels) {
    if (s_led_task) return;
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "ledbright", &v) == ESP_OK && v <= 100) s_led_bright = v;
        if (nvs_get_u8(h, "ledstealth", &v) == ESP_OK) s_led_stealth = v != 0;
        display_set_stealth(s_led_stealth);
        lcd_set_stealth(s_led_stealth);
        nvs_close(h);
    }
    rmt_tx_channel_config_t cc = led_channel_config(gpio);
    rmt_bytes_encoder_config_t ec = {
        .bit0 = { .level0 = 1, .duration0 = 10, .level1 = 0, .duration1 = 38 },
        .bit1 = { .level0 = 1, .duration0 = 28, .level1 = 0, .duration1 = 20 },
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cc, &s_led_chan) != ESP_OK || rmt_new_bytes_encoder(&ec, &s_led_enc) != ESP_OK ||
        rmt_enable(s_led_chan) != ESP_OK) {
        ESP_LOGW(TAG, "LED ring on GPIO %d unavailable", gpio);
        return;
    }
    s_led_gpio = gpio;
    s_led_n = pixels > LED_MAX ? LED_MAX : pixels;
    if (xTaskCreate(led_task, "wd_led", 4096, NULL, 1, &s_led_task) != pdPASS) { s_led_n = 0; ESP_LOGW(TAG, "LED task create failed"); }
}

// The board turned out to be another model at run time (a co-processor hello naming it): drop a ring it doesn't
// have. The task blanks the pixels and ends itself, then its RMT channel and encoder are freed.
static void led_stop(void) {
    if (!s_led_task) return;
    s_led_quit = true;
    for (int i = 0; i < 30 && s_led_quit; i++) vTaskDelay(pdMS_TO_TICKS(LED_FRAME_MS));
    if (s_led_quit) { s_led_quit = false; ESP_LOGW(TAG, "LED task didn't stop"); return; }
    vTaskDelay(pdMS_TO_TICKS(2));   // let it finish deleting itself
    if (!s_led_paused) { rmt_disable(s_led_chan); rmt_del_channel(s_led_chan); }
    rmt_del_encoder(s_led_enc);
    s_led_chan = NULL; s_led_enc = NULL; s_led_task = NULL;
    s_led_paused = false;
    s_led_fx = LED_IDLE;
    ESP_LOGI(TAG, "LED ring on GPIO %d off", s_led_gpio);
    s_led_n = 0; s_led_gpio = -1;
}

// Run the ring a board identity calls for, now: start it, stop it, or restart it on another pin or pixel count.
static void led_apply(int gpio, int pixels) {
    if (pixels > LED_MAX) pixels = LED_MAX;
    if (s_led_task && (pixels <= 0 || gpio != s_led_gpio || pixels != s_led_n)) led_stop();
    if (pixels > 0 && !s_led_task) led_start(gpio, pixels);
}

// `led <event> [rrggbb] [seconds]`, `led bright <0-100>`, `led stealth on|off`, `led` alone for the state.
static void cmd_led(const char *arg) {
    if (!s_led_n && !lcd_running()) { send_line("no LED ring on this board"); return; }   // a screen takes alerts too
    char ev[12] = "", a1[16] = "", a2[16] = "", a3[16] = "", a4[16] = "";
    sscanf(arg, "%11s %15s %15s %15s %15s", ev, a1, a2, a3, a4);
    if (!strcmp(ev, "alert")) {   // `led alert <pattern> <rrggbb> [seconds] [label]`: same as `led <pattern> ...`
        memcpy(ev, a1, sizeof(ev) - 1); ev[sizeof(ev) - 1] = '\0';
        memcpy(a1, a2, sizeof(a1)); memcpy(a2, a3, sizeof(a2)); memcpy(a3, a4, sizeof(a3));
    }
    if (!ev[0]) {
        send_line("led ring %d px on GPIO %d  ·  brightness %u%%  ·  stealth %s", s_led_n, s_led_gpio,
                  (unsigned)s_led_bright, s_led_stealth ? "on" : "off");
        return;
    }
    if (!strcmp(ev, "bright")) {
        int v = atoi(a1);
        s_led_bright = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        led_save();
        send_line("led brightness %u%%", (unsigned)s_led_bright);
        return;
    }
    if (!strcmp(ev, "stealth")) {
        if (!strcmp(a1, "on") || !strcmp(a1, "off")) { s_led_stealth = !strcmp(a1, "on"); led_save(); }
        display_set_stealth(s_led_stealth);
        lcd_set_stealth(s_led_stealth);
        send_line("led stealth %s", s_led_stealth ? "on" : "off");
        return;
    }
    if (!strcmp(ev, "off")) { s_led_fx = LED_IDLE; display_alert("", 0); lcd_alert("", 0, false, 0, false); send_line("led idle"); return; }
    static const struct { const char *name; led_fx_t fx; uint32_t rgb, secs; } FX[] = {
        { "police", LED_POLICE, 0, 8 },           { "heartbeat", LED_HEARTBEAT, 0xFF4FA3, 6 },
        { "strobe", LED_STROBE, 0xFF3B30, 6 },    { "spin", LED_SPIN, 0x00C8FF, 6 },
        { "pulse", LED_PULSE, 0xFFC800, 6 },      { "sparkle", LED_SPARKLE, 0xFFAA00, 3 },
        { "rainbow", LED_STORM, 0, 2 },           { "test", LED_TEST, 0, 4 },
        // earlier names
        { "drone", LED_SPIN, 0x00C8FF, 6 },       { "notable", LED_SPIN, 0xFFC800, 6 },
        { "capture", LED_SPARKLE, 0xFFAA00, 2 },  { "storm", LED_STORM, 0, 2 },
    };
    for (size_t i = 0; i < sizeof(FX) / sizeof(FX[0]); i++) {
        if (strcmp(ev, FX[i].name)) continue;
        uint32_t rgb = FX[i].rgb, secs = FX[i].secs;
        const char *next[3] = { a1, a2, a3 };
        int k = 0;
        if (strlen(a1) == 6 && strspn(a1, "0123456789abcdefABCDEF") == 6) { rgb = (uint32_t)strtoul(a1, NULL, 16); k = 1; }
        if (k < 3 && next[k][0] >= '0' && next[k][0] <= '9') {
            long s = atol(next[k++]);
            secs = (uint32_t)(s < 1 ? 1 : s > 60 ? 60 : s);
        }
        uint32_t now = led_ms();
        s_led_rgb = rgb; s_led_since = now; s_led_until = now + secs * 1000;
        s_led_fx = FX[i].fx;
        display_alert(k < 3 && next[k][0] ? next[k] : FX[i].name, secs * 1000);   // the app's label, or the pattern's name
        lcd_alert(k < 3 && next[k][0] ? next[k] : FX[i].name, secs * 1000, false, rgb, FX[i].fx == LED_POLICE);
        send_line("led %s %06lx %lus", FX[i].name, (unsigned long)rgb, (unsigned long)secs);
        return;
    }
    send_line("led alert police|heartbeat|strobe|spin|pulse|sparkle|rainbow <rrggbb> [seconds] [label]  ·  led test  ·  led off  ·  led bright 0-100  ·  led stealth on|off");
}
#else
static void led_start(int gpio, int pixels) { (void)gpio; (void)pixels; }
static void led_apply(int gpio, int pixels) { (void)gpio; (void)pixels; }
static void led_storm(void) {}
static void cmd_led(const char *arg) { (void)arg; send_line("no LED ring on this board"); }
#endif

#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32S3
static i2c_master_bus_handle_t s_i2c_bus;

// An I2C bus has both lines pulled up. A board with 4/5 free, or a GPS module on 4, reads low here against the
// internal pull-down, so the probe never clocks a pin it doesn't own.
static bool i2c_pulled_up(int sda, int scl) {
    gpio_config_t io = { .pin_bit_mask = (1ULL << sda) | (1ULL << scl), .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(50);
    bool up = gpio_get_level(sda) && gpio_get_level(scl);
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    return up;
}

// Every address that answers, once, in the boot log: future boards get fingerprinted from it.
static void i2c_scan_log(int sda, int scl) {
    char found[120] = "";
    int len = 0;
    for (uint16_t a = 0x08; a < 0x78; a++)
        if (len < (int)sizeof(found) - 6 && i2c_master_probe(s_i2c_bus, a, 10) == ESP_OK)
            len += snprintf(found + len, sizeof(found) - len, " 0x%02x", a);
    ESP_LOGI(TAG, "I2C on SDA %d / SCL %d:%s", sda, scl, len ? found : " nothing answered");
}

#endif

#if CONFIG_IDF_TARGET_ESP32C5
// ── MAX17048 battery gauge on I2C SDA 5 / SCL 4 (the Biscuit Crumb's wiring) ──
#define CRUMB_SDA      5
#define CRUMB_SCL      4
#define MAX17048_ADDR  0x36

static i2c_master_dev_handle_t s_gauge;
static bool s_gauge_i2c;   // the Crumb's gauge answered on 5/4 (s_has_batt can also mean a co-processor's gauge)

static bool gauge_read(uint8_t reg, uint16_t *out) {
    uint8_t v[2];
    if (i2c_master_transmit_receive(s_gauge, &reg, 1, v, 2, 50) != ESP_OK) return false;
    *out = (uint16_t)((v[0] << 8) | v[1]);
    return true;
}

static void battery_read(void) {
    uint16_t soc, vcell;
    if (!s_gauge_i2c) return;
    if (gauge_read(0x04, &soc)) s_batt_pct = (soc >> 8) > 100 ? 100 : (soc >> 8);   // whole percent in the high byte
    if (gauge_read(0x02, &vcell)) s_batt_mv = (int)((uint32_t)vcell * 5 / 64);       // 78.125 µV per bit
}

#if CONFIG_WARDRIVEESP_BENCH_TOOLS
// Bench builds: log what answers on another candidate pair (0/1: the Rabbit Labs Poltergeist's display bus), only
// when both lines read pulled up.
static void i2c_pair_scan(int sda, int scl) {
    if (!i2c_pulled_up(sda, scl)) { ESP_LOGI(TAG, "I2C on SDA %d / SCL %d: no pull-ups, not probed", sda, scl); return; }
    i2c_master_bus_handle_t keep = s_i2c_bus;
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = sda, .scl_io_num = scl,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7 };
    if (i2c_new_master_bus(&bc, &s_i2c_bus) == ESP_OK) {
        i2c_scan_log(sda, scl);
        i2c_del_master_bus(s_i2c_bus);
    }
    s_i2c_bus = keep;
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
}
#endif

/// True when the battery gauge answers; it stays open for battery_read.
static bool crumb_gauge_probe(void) {
    if (!i2c_pulled_up(CRUMB_SDA, CRUMB_SCL)) return false;
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = CRUMB_SDA, .scl_io_num = CRUMB_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7 };
    if (i2c_new_master_bus(&bc, &s_i2c_bus) != ESP_OK) return false;
    i2c_scan_log(CRUMB_SDA, CRUMB_SCL);
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MAX17048_ADDR, .scl_speed_hz = 100000 };
    uint16_t ver = 0;
    if (i2c_master_probe(s_i2c_bus, MAX17048_ADDR, 50) == ESP_OK &&
        i2c_master_bus_add_device(s_i2c_bus, &dc, &s_gauge) == ESP_OK &&
        gauge_read(0x08, &ver) && (ver & 0xFFF0) == 0x0010) {
        s_has_batt = s_gauge_i2c = true;
        battery_read();
        ESP_LOGI(TAG, "battery gauge MAX17048 v%04x: %d%%, %d mV", ver, s_batt_pct, s_batt_mv);
        return true;
    }
    if (s_gauge) { i2c_master_bus_rm_device(s_gauge); s_gauge = NULL; }
    i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
    gpio_reset_pin(CRUMB_SDA);
    gpio_reset_pin(CRUMB_SCL);
    return false;
}
#else
static bool crumb_gauge_probe(void) { return false; }
static void battery_read(void) {}
#endif

#if CONFIG_IDF_TARGET_ESP32S3
// ── Waveshare ESP32-S3 Touch AMOLED 2.06 (the watch) ──
// Board bus SDA 15 / SCL 14 (Waveshare BSP esp32_s3_touch_amoled_2_06.h, board_config/pin_config.h). The AXP2101 power
// chip at 0x34 reads back its IC type (register 0x03, 0x4A: XPowersLib AXP2101Constants.h), and the rest of the bus
// answers at boot with no pin driven: ES8311 codec 0x18 and ES7210 mic ADC 0x40 (the BSP's esp_codec_dev addresses
// 0x30 / 0x80 in 8-bit form), PCF85063 clock 0x51 and QMI8658 motion sensor 0x6B (SensorLib). The FT3168 touch
// controller (0x38) is not part of it: it stays silent until its reset line is pulsed (bench 2026-11-03).
#define WATCH_SDA            15
#define WATCH_SCL            14
#define WATCH_I2C_HZ         400000
#define AXP2101_ADDR         0x34
#define AXP2101_REG_IC_TYPE  0x03
#define AXP2101_IC_TYPE      0x4A
static const uint8_t WATCH_PARTS[] = { 0x18, 0x40, 0x51, 0x6B };
// Pins the watch's own hardware holds: AMOLED QSPI data 4-7, reset 8, clock 11, select 12; touch reset 9 and
// interrupt 38; the board bus 14/15.
static const int8_t WATCH_PINS[] = { 4, 5, 6, 7, 8, 9, 11, 12, 14, 15, 38 };
static i2c_master_dev_handle_t s_pmic;
static bool s_watch;

/// True when the AXP2101 (by IC type) and every other part of the watch's board bus answer on 15/14. The bus and the
/// power chip stay open.
static bool watch_probe(void) {
    if (!i2c_pulled_up(WATCH_SDA, WATCH_SCL)) return false;
    i2c_master_bus_config_t bc = { .i2c_port = -1, .sda_io_num = WATCH_SDA, .scl_io_num = WATCH_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7 };
    if (i2c_new_master_bus(&bc, &s_i2c_bus) != ESP_OK) return false;
    i2c_scan_log(WATCH_SDA, WATCH_SCL);
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP2101_ADDR,
                               .scl_speed_hz = WATCH_I2C_HZ };
    uint8_t reg = AXP2101_REG_IC_TYPE, type = 0;
    bool pmic = i2c_master_probe(s_i2c_bus, AXP2101_ADDR, 50) == ESP_OK &&
                i2c_master_bus_add_device(s_i2c_bus, &dc, &s_pmic) == ESP_OK &&
                i2c_master_transmit_receive(s_pmic, &reg, 1, &type, 1, 50) == ESP_OK && type == AXP2101_IC_TYPE;
    size_t parts = 0;
    for (size_t i = 0; pmic && i < sizeof(WATCH_PARTS); i++)
        if (i2c_master_probe(s_i2c_bus, WATCH_PARTS[i], 50) == ESP_OK) parts++;
    ESP_LOGI(TAG, "watch probe: AXP2101 IC type %02x, %u of %u board parts answered", type, (unsigned)parts,
             (unsigned)sizeof(WATCH_PARTS));
    if (pmic && parts == sizeof(WATCH_PARTS)) return true;
    if (s_pmic) { i2c_master_bus_rm_device(s_pmic); s_pmic = NULL; }
    i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
    gpio_reset_pin(WATCH_SDA);
    gpio_reset_pin(WATCH_SCL);
    return false;
}
#endif

/// A pin the board's own hardware uses, so a GPS probe must leave it alone (the Crumb's gauge bus, the watch's screen).
static bool board_pin_busy(int gpio) {
#if CONFIG_IDF_TARGET_ESP32C5
    if (s_gauge_i2c && (gpio == CRUMB_SDA || gpio == CRUMB_SCL)) return true;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_watch)
        for (size_t i = 0; i < sizeof(WATCH_PINS); i++) if (WATCH_PINS[i] == gpio) return true;
#endif
    return gpio < 0;
}

#if CONFIG_WARDRIVEESP_BENCH_TOOLS
// `pinsense <gpio>` (bench builds): read a pin with the internal pull-up, then the pull-down, never driving it. A pin
// wired to something that pulls it (a WS2812 input, a pull resistor) reads differently from a floating one.
static bool pin_off_limits(int gpio) {
#if CONFIG_IDF_TARGET_ESP32C5
    static const int8_t NO[] = { 2, 7, 13, 14, 25, 26, 27, 28 };   // strapping, USB D-/D+
#elif CONFIG_IDF_TARGET_ESP32C6
    static const int8_t NO[] = { 4, 5, 8, 9, 12, 13, 15 };
#elif CONFIG_IDF_TARGET_ESP32S3
    static const int8_t NO[] = { 0, 3, 19, 20, 45, 46 };
#else
    static const int8_t NO[] = { 2, 8, 9, 18, 19 };
#endif
    if (!GPIO_IS_VALID_GPIO(gpio)) return true;
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
    if (s_led_task && gpio == s_led_gpio) return false;   // the ring's own pin: reserved by its RMT channel, handed over below
#endif
    if (esp_gpio_is_reserved(BIT64(gpio))) return true;   // flash / PSRAM / a pin a driver holds
    if (gpio == U0TXD_GPIO_NUM || gpio == U0RXD_GPIO_NUM) return true;                 // console / USB-UART link
    for (size_t i = 0; i < sizeof(NO); i++) if (NO[i] == gpio) return true;
#if CONFIG_IDF_TARGET_ESP32C5
    if (s_gauge_i2c && (gpio == 4 || gpio == 5)) return true;                           // the gauge bus
#endif
    return false;
}

static int pin_read(int gpio, bool pull_up) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                         .pull_down_en = pull_up ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE };
    gpio_config(&io);
    esp_rom_delay_us(200);
    int high = 0;
    for (int i = 0; i < 5; i++) { high += gpio_get_level(gpio); esp_rom_delay_us(100); }
    return high;   // of 5 samples
}

static void cmd_pinsense(const char *arg) {
    int gpio = atoi(arg);
    if (!*arg || pin_off_limits(gpio)) { send_line("pinsense: GPIO %s is off limits on %s", *arg ? arg : "?", CONFIG_IDF_TARGET); return; }
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
    bool ring = s_led_task && gpio == s_led_gpio;
    if (ring) {   // hand the pin over: stop the ring's RMT channel, put it back afterwards
        s_led_paused = true;
        vTaskDelay(pdMS_TO_TICKS(80));
        rmt_disable(s_led_chan);
        rmt_del_channel(s_led_chan);
    }
#endif
    int pu = pin_read(gpio, true), pd = pin_read(gpio, false);
    gpio_reset_pin(gpio);
#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
    if (ring) {
        rmt_tx_channel_config_t cc = led_channel_config(gpio);
        if (rmt_new_tx_channel(&cc, &s_led_chan) == ESP_OK && rmt_enable(s_led_chan) == ESP_OK) s_led_paused = false;
        else send_line("pinsense: the ring didn't come back, reboot to restore it");
    }
#endif
    send_line("pinsense %d pu=%d pd=%d  (high samples of 5: pull-up %d, pull-down %d)", gpio, pu >= 3, pd >= 3, pu, pd);
}
#endif

// A board identity set with `board <id>` for boards whose part can't be read back (NVS "boardid"). Still gated on the
// hardware: a stored Mini only counts when the Mini's bus chip select reads pulled up.
static bool stored_board_is(const char *id) {
    nvs_handle_t h;
    char v[24] = "";
    size_t n = sizeof(v);
    if (nvs_open("iosesp", NVS_READONLY, &h) != ESP_OK) return false;
    bool got = nvs_get_str(h, "boardid", v, &n) == ESP_OK;
    nvs_close(h);
    return got && !strcmp(v, id);
}

// Which board this is, from a hardware fingerprint only: a part that answers and that no other board has, each one
// confirmed on the bench (docs/device-research). Anything not listed is BOARD_NONE.
//   Biscuit Crumb   ESP32-C5 with a MAX17048 battery gauge on SDA 5 / SCL 4 (the Biscuit Ultra's gauge sits on its
//                   other chip, never on its C5)
//   Koko V8         ESP32-C5 with the same MAX17048 on 5/4, plus an ILI9341 screen that answers its ID read
//   Mini v3         ESP32-C5 without a gauge, with an ST7735 screen that answers its ID read
//   Ghost Board     no fingerprint yet
#if CONFIG_IDF_TARGET_ESP32C5
#define POLT_SDA 0   // the Poltergeist's SSD1306 bus
#define POLT_SCL 1
static bool s_has_disp;
static bool s_has_lcd;   // an ILI9341 answered on the Koko V8's bus (Koko V8 with its gauge, AWOK Dual C5 without)
static bool s_has_mini;  // the Mini v3's ST7735 answered (or the board was told it is one)
static uint32_t flash_physical_mb(void) {
    uint32_t sz = 0;
    return esp_flash_get_physical_size(NULL, &sz) == ESP_OK ? sz >> 20 : 0;
}
#endif

static board_t board_detect(void) {
#if CONFIG_IDF_TARGET_ESP32C5
    if (s_gauge_i2c) return s_has_lcd ? BOARD_KOKO_V8 : BOARD_CRUMB;
    if (s_has_mini) return BOARD_MINI_V3;
    if (s_has_lcd) return BOARD_AWOK_DUAL_C5;
    if (s_has_disp && flash_physical_mb() == 16) return BOARD_POLTERGEIST;   // SSD1306 on 0/1, 16 MB, no gauge on 5/4
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_watch) return BOARD_WAVESHARE_AMOLED_206;
#endif
    return BOARD_NONE;
}

// The app taking offline runs off the card (`sd read`, then `sd del`), for the color screens' transfer screen. The
// command path only stores a few numbers here (never a wait, never the bus); the screen task reads them on its own frame.
#define SDX_IDLE_MS  8000    // no read this long while a file is still coming: the app stopped, back to the dashboard
#define SDX_SAVE_MS  30000   // every byte read: the phone imports it before `sd del`
#define SDX_SENT_MS  2500    // how long SENT shows after the last run is deleted
static struct {
    volatile uint32_t t_last;        // ms of the last read (0 = no transfer)
    volatile uint32_t t_file;        // ms of the first read of this file
    volatile uint32_t off0;          // where this file's reads started (a resume starts past 0)
    volatile uint32_t end, size;     // the furthest byte read, the file's size
    volatile uint32_t taken_bytes;   // runs deleted this time, in bytes
    volatile uint32_t sent_at;       // ms SENT started (0 = not showing)
    volatile uint8_t  run, runs, taken;
    char file[24];
} s_sdx;

static uint32_t sdx_now(void) { uint32_t t = (uint32_t)(esp_timer_get_time() / 1000); return t ? t : 1; }

/// `sd read` answered `n` bytes of `name` at `off`.
static void sdx_read(const char *name, uint32_t off, int n, uint32_t size) {
    uint32_t now = sdx_now();
    if (!s_sdx.t_last || now - s_sdx.t_last > SDX_SAVE_MS) {   // a new take
        uint32_t wn = 0, wb = 0;
        sdrun_pending(&wn, &wb);
        s_sdx.runs = (uint8_t)(wn > 250 ? 250 : wn ? wn : 1);
        s_sdx.taken = 0;
        s_sdx.taken_bytes = 0;
        s_sdx.file[0] = 0;
    }
    s_sdx.sent_at = 0;
    if (strcmp(s_sdx.file, name)) {
        strlcpy(s_sdx.file, name, sizeof(s_sdx.file));
        s_sdx.run = (uint8_t)(s_sdx.taken < 250 ? s_sdx.taken + 1 : 250);
        if (s_sdx.run > s_sdx.runs) s_sdx.runs = s_sdx.run;
        s_sdx.off0 = off;
        s_sdx.end = 0;
        s_sdx.t_file = now;
    }
    if (off < s_sdx.off0) s_sdx.off0 = off;
    if (n > 0 && off + (uint32_t)n > s_sdx.end) s_sdx.end = off + (uint32_t)n;
    if (n == 0 && off >= size) s_sdx.end = size;   // asked past the end: all of it is on the phone
    s_sdx.size = size;
    s_sdx.t_last = now;
}

/// `sd del` took `name` off the card: when nothing is left waiting, SENT.
static void sdx_deleted(const char *name) {
    if (!s_sdx.t_last) return;   // a delete with no reads this time (the app already held the file): no screen
    if (!strcmp(s_sdx.file, name)) { s_sdx.taken_bytes += s_sdx.size; s_sdx.file[0] = 0; }
    if (s_sdx.taken < 250) s_sdx.taken++;
    uint32_t wn = 0, wb = 0;
    sdrun_pending(&wn, &wb);
    if (s_sdx.taken + wn > s_sdx.runs) s_sdx.runs = (uint8_t)(s_sdx.taken + wn > 250 ? 250 : s_sdx.taken + wn);
    if (!wn) { s_sdx.t_last = 0; s_sdx.sent_at = sdx_now(); }
    else s_sdx.t_last = sdx_now();   // the next run's reads come shortly
}

#if CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32S3
// What the transfer screen shows (lcd_fill): only while a host holds the board.
static void sdx_fill(lcd_status_t *st) {
    uint32_t now = sdx_now(), last = s_sdx.t_last, sent = s_sdx.sent_at;
    if (!host_linked()) return;
    if (sent && now - sent < SDX_SENT_MS) {
        st->sd_sent = true;
        st->sd_sent_ms = now - sent;
        st->sd_runs = s_sdx.taken;
        st->sd_sent_bytes = s_sdx.taken_bytes;
        return;
    }
    uint32_t end = s_sdx.end, size = s_sdx.size;
    if (!last || !s_sdx.file[0] || now - last > (size && end >= size ? SDX_SAVE_MS : SDX_IDLE_MS)) return;
    st->sd_send = true;
    st->sd_run = s_sdx.run;
    st->sd_runs = s_sdx.runs;
    strlcpy(st->sd_file, s_sdx.file, sizeof(st->sd_file));
    st->sd_bytes = end;
    st->sd_size = size;
    uint32_t ms = last - s_sdx.t_file, got = end > s_sdx.off0 ? end - s_sdx.off0 : 0;
    st->sd_rate = ms >= 1000 ? (uint32_t)((uint64_t)got * 1000 / ms) : 0;
}

// A firmware update in progress, for the screens: they show its progress instead of the dashboard until the restart.
#define OTA_FILL(st) do { \
        if (s_ota_active) { \
            uint32_t sz = s_ota_size, wr = s_ota_written; \
            (st)->ota = true; \
            (st)->ota_installing = s_ota_end_req; \
            (st)->ota_pct = (uint8_t)(sz ? (uint64_t)(wr > sz ? sz : wr) * 100 / sz : 0); \
            (st)->ota_kb = wr / 1024; \
            (st)->ota_total_kb = sz / 1024; \
        } \
    } while (0)

#if CONFIG_IDF_TARGET_ESP32C5
// What the Poltergeist's screen shows, a few times a second.
static void display_fill(display_status_t *st) {
    st->channel = s_crumb_ch;
    const char *m = role_name(s_role);
    for (int i = 0; m[i] && i < (int)sizeof(st->mode) - 1; i++) st->mode[i] = (char)(m[i] - 32 * (m[i] >= 'a' && m[i] <= 'z'));
    st->ble = s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
    st->usb = s_usb_sessions != 0;
    strlcpy(st->holder, s_holder, sizeof(st->holder));
    st->captures = s_cap_hs + s_cap_pmkid;
    st->bt_devices = s_bt_last;
    st->aps_live = apc_live((uint32_t)(esp_timer_get_time() / 1000));
    st->aps_session = apc_session();
    OTA_FILL(st);
}
#endif

// What the color screens (Koko V8, Mini v3, AWOK, the watch) show, a few times a second.
static void lcd_fill(lcd_status_t *st) {
    st->channel = s_crumb_ch;
    const char *m = role_name(s_role);
    for (int i = 0; m[i] && i < (int)sizeof(st->mode) - 1; i++) st->mode[i] = (char)(m[i] - 32 * (m[i] >= 'a' && m[i] <= 'z'));
    st->ble = s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
    st->usb = s_usb_sessions != 0;
    strlcpy(st->holder, s_holder, sizeof(st->holder));
    st->captures = s_cap_hs + s_cap_pmkid;
    st->bt_devices = s_bt_last;
    st->batt_pct = s_has_batt ? s_batt_pct : -1;
    st->aps_live = apc_live((uint32_t)(esp_timer_get_time() / 1000));
    st->aps_session = apc_session();
    st->sd_log = sdrun_active() && !host_linked();
    st->sd_rows = sdrun_rows();
    sdx_fill(st);
    OTA_FILL(st);
}
#endif

/// CAPS: board=<id> board_name=<name, spaces as '_'>, or nothing for an unidentified board.
static void board_caps_set(const board_def_t *b) {
    char caps[sizeof(s_board_caps)] = "";
    if (b->id) {
        int n = snprintf(caps, sizeof(caps), "board=%s board_name=%s ", b->id, b->name);
        for (int i = (int)strlen(b->id) + 18; i < n - 1; i++) if (caps[i] == ' ') caps[i] = '_';
    }
    strlcpy(s_board_caps, caps, sizeof(s_board_caps));
}

// Boot: the battery gauge (if any), the board's identity, and its LED ring when the identity names one.
static void board_init(void) {
    heap_check("boot: before gauge probe");
    (void)crumb_gauge_probe();   // battery level and the I2C scan, whatever the board turns out to be
    heap_check("boot: after gauge probe");
#if CONFIG_IDF_TARGET_ESP32S3
    s_watch = watch_probe();
#endif
#if CONFIG_IDF_TARGET_ESP32C5
    s_has_disp = !s_gauge_i2c && display_probe(POLT_SDA, POLT_SCL);
    s_has_lcd = s_gauge_i2c && lcd_probe_koko();
    s_has_mini = !s_gauge_i2c && !s_has_disp && lcd_probe_mini(stored_board_is("marauder_mini_v3"));
    // No gauge, no Mini: an ILI9341 that answers its ID read on the same pulled-up bus (AWOK Dual C5's screen chip).
    if (!s_gauge_i2c && !s_has_disp && !s_has_mini) s_has_lcd = lcd_probe_koko();
#if CONFIG_WARDRIVEESP_BENCH_TOOLS
    if (!s_has_disp) i2c_pair_scan(0, 1);
#endif
#endif
    s_board = board_detect();
#if CONFIG_IDF_TARGET_ESP32C5
    if (s_has_disp && s_board != BOARD_POLTERGEIST) { display_release(); s_has_disp = false; }   // not ours to drive
#endif
    const board_def_t *b = &BOARDS[s_board];
#if CONFIG_WARDRIVEESP_BENCH_RING
    // Bench builds only: drive the configured ring without an identity, to test the effects on a known board.
    if (!b->pixels) {
        static const board_def_t bench = { NULL, "bench ring", CONFIG_WARDRIVEESP_BENCH_RING_GPIO, CONFIG_WARDRIVEESP_BENCH_RING_PIXELS };
        b = &bench;
    }
#endif
    ESP_LOGI(TAG, "board: %s", b->name);
    board_caps_set(b);
    if (b->pixels) led_start(b->led_gpio, b->pixels);
#if CONFIG_IDF_TARGET_ESP32C6
    // Ghost Board: built-in GPS (GPIO 2) and a microSD slot on its own bus (GhostESP's sdkconfig.ghostboard: SPI CS 23,
    // CLK 19, MISO 20, MOSI 18).
    if (s_board == BOARD_GHOSTBOARD) sdrun_init_bus(b->id, b->name, FW_VERSION, 19, 20, 18, 23);
#endif
#if CONFIG_IDF_TARGET_ESP32C5
    if (s_board == BOARD_POLTERGEIST) display_start(b->name, FW_VERSION, display_fill);
    if (s_board == BOARD_KOKO_V8 || s_board == BOARD_MINI_V3 || s_board == BOARD_AWOK_DUAL_C5) {
        lcd_start(b->name, FW_VERSION, lcd_fill);
        sdrun_init(b->id, b->name, FW_VERSION);   // the card mounts in the background (the slot on the screen's bus)
        nvs_handle_t h;   // no ring to load it: the screen takes the saved stealth setting itself
        uint8_t v;
        if (nvs_open("iosesp", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, "ledstealth", &v) == ESP_OK) { s_led_stealth = v != 0; lcd_set_stealth(s_led_stealth); }
            nvs_close(h);
        }
    }
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    if (s_board == BOARD_WAVESHARE_AMOLED_206 && lcd_open_amoled()) lcd_start(b->name, FW_VERSION, lcd_fill);
#endif
    heap_check("boot: after LED start");
}

static void screen_line(int page, int x, const char *hex) { send_line("WD:SCREEN page=%d x=%d %s", page, x, hex); }
/// `screen`: the panel's current frame, half a page per line (a mirror for the app and for remote checks).
static void cmd_screen(void) {
    if (lcd_running()) { send_line("screen %s color (no mirror)", lcd_size()); return; }
    if (!display_running()) { send_line("no screen on this board"); return; }
    display_dump(screen_line);
}

/// `board`: the identity in use. `board marauder_mini_v3`: store it for a board whose screen can't be read back (applies
/// at the next boot, and only when the hardware gate passes). `board auto`: forget it.
static void cmd_board(const char *arg) {
    if (!*arg) {
        send_line("board %s%s", BOARDS[s_board].name, stored_board_is("marauder_mini_v3") ? "  ·  stored: marauder_mini_v3" : "");
        return;
    }
    bool clear = !strcmp(arg, "auto");
    if (!clear && strcmp(arg, "marauder_mini_v3")) { send_line("board marauder_mini_v3 | board auto"); return; }
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READWRITE, &h) != ESP_OK) { send_line("board: storage unavailable"); return; }
    if (clear) nvs_erase_key(h, "boardid");
    else nvs_set_str(h, "boardid", arg);
    nvs_commit(h);
    nvs_close(h);
    send_line("board %s stored  ·  reboot to apply", clear ? "auto" : arg);
}

static void cmd_help(void) {
    send_line("WardriveESP %s commands:", FW_VERSION);
    send_line("  info          firmware, chip, memory, uptime");
    send_line("  status        scanning, sweeps, channels");
    send_line("  link          what it's connected to (phone address, signal, timing)");
    send_line("  drop          disconnect the phone so another one can link");
    send_line("  sd [status|mount|test|list|read <file> <off> <len>|del <file>]  the microSD card (offline runs)");
    send_line("  ch            show channels  ·  ch 1,6,11  ·  ch all");
    send_line("  start | stop  resume or pause scanning");
    send_line("  bt on|off     scan Bluetooth instead of Wi-Fi");
    send_line("  cap on|off    passive capture (handshakes, PMKIDs, clients)");
#if CSI_SUPPORTED
    send_line("  csi start ch=N [mac=AA:BB:CC:DD:EE:FF] [rate=1-100] [sc=8-64]   Wi-Fi radar (CSI)");
    send_line("  csi tx ch=N [rate=1-100] [id=N] [via=espnow|raw]  be the radar's transmitter  ·  csi stop  back to the previous mode");
    send_line("  csi mesh ch=N id=N [rate=1-100] [slot=I slots=N] [report=50-1000]  multi-board radar (each board sends and listens)");
#endif
    if (s_led_n) {
        send_line("  led           ring state  ·  led alert <pattern> <rrggbb> [seconds]  ·  led test  ·  led off");
        send_line("                patterns: police heartbeat strobe spin pulse sparkle rainbow");
        send_line("  led bright N  ring brightness 0-100  ·  led stealth on|off  keeps the ring dark");
    }
    if (s_cp_caps[0]) send_line("  coproc        second radio  ·  coproc mode btc|wifi|off  (Bluetooth Classic, Wi-Fi 2.4 GHz)");
    send_line("  reboot        restart the board");
}

static void cmd_info(void) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    const esp_partition_t *run = esp_ota_get_running_partition();
    unsigned long up = (unsigned long)(esp_timer_get_time() / 1000000);
    send_line("WardriveESP %s%s  ·  %s rev %d.%d  ·  %s", FW_VERSION, FW_TAG, CONFIG_IDF_TARGET,
              ci.revision / 100, ci.revision % 100, s_name);
    if (s_has_batt)
        send_line("board %s  ·  LED ring %d px  ·  battery %d%% (%d mV)%s%s", BOARDS[s_board].name, s_led_n,
                  s_batt_pct, s_batt_mv, lcd_running() ? "  ·  screen " : "", lcd_size());
    else if (s_board || s_led_n)
        send_line("board %s  ·  LED ring %d px%s%s%s", BOARDS[s_board].name, s_led_n, display_running() ? "  ·  screen 128x32" : "",
                  lcd_running() ? "  ·  screen " : "", lcd_size());
    send_line("Wi-Fi MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    send_line("uptime %luh %02lum %02lus  ·  last reset: %s", up / 3600, (up / 60) % 60, up % 60,
              reset_reason_name(esp_reset_reason()));
    send_line("memory %lu KB free (low %lu KB)  ·  flash %lu MB",
              (unsigned long)(esp_get_free_heap_size() / 1024),
              (unsigned long)(esp_get_minimum_free_heap_size() / 1024),
              (unsigned long)(flash / (1024 * 1024)));
    send_line("slot %s  ·  %s  ·  %u channels", run ? run->label : "?",
              s_has_5g ? "2.4 + 5 GHz" : "2.4 GHz", (unsigned)s_supported_n);
    coproc_info();
    send_line("%s", s_boot_line);
    if (s_crash_line[0]) send_line("%s", s_crash_line);
}

static const char *disc_reason_name(uint32_t r) {
    switch (r & 0xff) {
    case 0x08: return "signal lost (timeout)";
    case 0x13: return "phone disconnected";
    case 0x16: return "board disconnected";
    case 0x3e: return "link setup failed";
    default:   return "other";
    }
}

static void cmd_link(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    struct ble_gap_conn_desc d;
    if (!s_ble_synced) {
        send_line("Bluetooth: off while a USB host has the board (or still starting)");
    } else if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(s_conn_handle, &d) != 0) {
        send_line("Bluetooth: not connected  ·  advertising %s as %s", ble_gap_adv_active() ? "yes" : "NO", s_name);
    } else {
        const uint8_t *a = d.peer_id_addr.val;   // little-endian
        int8_t rssi = 0;
        char sig[16] = "?";
        if (ble_gap_conn_rssi(s_conn_handle, &rssi) == 0) snprintf(sig, sizeof(sig), "%d dBm", rssi);
        uint32_t up = (now - s_conn_since_ms) / 1000;
        send_line("Bluetooth: connected to %02X:%02X:%02X:%02X:%02X:%02X  ·  for %luh %02lum %02lus",
                  a[5], a[4], a[3], a[2], a[1], a[0],
                  (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
        send_line("  held by %s%s", s_holder[0] ? s_holder : "an app that didn't say (older version)",
                  s_holder[0] ? (s_holder_ios ? " (iPhone)" : " (Android)") : "");
        send_line("  signal %s  ·  MTU %u  ·  interval %u ms  ·  timeout %u ms  ·  stream %s",
                  sig, (unsigned)ble_att_mtu(s_conn_handle), (unsigned)(d.conn_itvl * 5 / 4),
                  (unsigned)(d.supervision_timeout * 10), s_notify_enabled ? "on" : "off");
    }
    if (s_last_disc_ms)
        send_line("  links since boot %lu  ·  last drop %lus ago: %s (0x%02lx)", (unsigned long)s_conns,
                  (unsigned long)((now - s_last_disc_ms) / 1000), disc_reason_name(s_last_disc_reason),
                  (unsigned long)(s_last_disc_reason & 0xff));
    else
        send_line("  links since boot %lu  ·  no drops", (unsigned long)s_conns);
}

static void cmd_drop(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) { send_line("no phone connected"); return; }
    send_line("disconnecting the phone · the board advertises again right after");
    (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static void cmd_status(void) {
    uint8_t plan[MAX_CHANNELS];
    size_t n = plan_copy(plan);
    char chans[200] = "";
    format_channel_list(chans, 0, sizeof(chans), plan, n);
    uint16_t mtu = s_bt_up && s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn_handle) : 0;
    if (s_role == ROLE_BT) {
        send_line("mode Bluetooth  ·  scanning %s  ·  %lu windows  ·  %lu devices last window",
                  s_bt_running ? "on" : (s_ota_active ? "paused (updating)" : "off"),
                  (unsigned long)s_bt_windows, (unsigned long)s_bt_last);
#if CSI_SUPPORTED
    } else if (s_role == ROLE_CSI) {
        static const char *const WHAT[] = { "capturing", "transmitting", "mesh" };
        send_line("mode CSI radar (%s)  ·  %s  ·  channel %u  ·  id %u  ·  %lu %s/s  ·  %lu dropped",
                  WHAT[s_csi_cfg.mode], s_csi_running ? "on" : (s_ota_active ? "paused (updating)" : "off"),
                  (unsigned)s_csi_cfg.ch, (unsigned)s_csi_cfg.id, (unsigned long)s_csi_pps,
                  s_csi_cfg.mode == CSI_MESH ? "links" : s_csi_cfg.mode == CSI_TX ? "frames" : "packets",
                  (unsigned long)s_csi_dropped);
#endif
    } else if (s_role == ROLE_CAP) {
        send_line("mode Capture  ·  scanning %s  ·  %lu APs  ·  %lu handshakes  ·  %lu pmkid  ·  %lu clients",
                  s_cap_running ? "on" : (s_ota_active ? "paused (updating)" : "off"),
                  (unsigned long)s_cap_aps, (unsigned long)s_cap_hs, (unsigned long)s_cap_pmkid,
                  (unsigned long)s_cap_clients);
        send_line("channels %s", n ? chans : "none");
    } else {
        send_line("mode Wi-Fi  ·  scanning %s  ·  %lu sweeps  ·  %lu APs last sweep",
                  s_streaming ? "on" : (s_ota_active ? "paused (updating)" : "off"),
                  (unsigned long)s_sweeps, (unsigned long)s_last_aps);
        send_line("channels %s", n ? chans : "none");
    }
    uint8_t u = s_usb_sessions;
    send_line("BLE MTU %u  ·  USB %s", (unsigned)mtu,
              (u & SINK_USB) && (u & SINK_UART) ? "usb+uart" : (u & SINK_USB) ? "usb" : (u & SINK_UART) ? "uart" : "off");
}

static void cmd_channels(const char *arg) {
    if (!*arg) {
        char sup[200] = "", cur[200] = "";
        uint8_t plan[MAX_CHANNELS];
        size_t n = plan_copy(plan);
        format_channel_list(sup, 0, sizeof(sup), s_supported, s_supported_n);
        format_channel_list(cur, 0, sizeof(cur), plan, n);
        send_line("supported %s", sup);
        send_line("scanning  %s", n ? cur : "none");
        return;
    }
    if (!strcmp(arg, "all")) {
        plan_reset_to_all();
        if (s_streaming) send_plan_line();
        send_line("scanning all %u channels", (unsigned)s_supported_n);
        return;
    }
    if (!apply_channel_list(arg)) { send_line("none of those channels are supported"); cmd_channels(""); return; }
    send_line("ok — the app re-splits channels when a board links or drops");
}

#if CSI_SUPPORTED
/// "AA:BB:CC:DD:EE:FF", "AA-BB-..." or "AABBCCDDEEFF".
static bool parse_mac(const char *s, uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        if (i && (*s == ':' || *s == '-')) s++;
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) return false;
        char h[3] = { s[0], s[1], '\0' };
        out[i] = (uint8_t)strtoul(h, NULL, 16);
        s += 2;
    }
    return *s == '\0' || *s == ' ';
}
#endif

// `csi` (state), `csi start ch=<n> [mac=<transmitter>] [rate=<1-100>] [sc=<8-64>]`, `csi tx ch=<n> [rate=<1-100>]
// [id=<0-255>]`, `csi mesh ch=<n> id=<0-255> [rate=<1-100>] [slot=<i> slots=<n>] [report=<50-1000>]`, `csi stop` (back
// to the role the board had before). Called under s_cmd_lock.
static void cmd_csi(const char *arg) {
#if !CSI_SUPPORTED
    (void)arg;
    send_line("WD:ERR reason=csi_unsupported chip=%s", CONFIG_IDF_TARGET);
#else
    char verb[8] = "";
    sscanf(arg, "%7s", verb);
    if (!verb[0]) {
        if (s_role == ROLE_CSI) { csi_cfg_to(SINK_ALL); cmd_status(); }
        else send_line("csi off  ·  csi start ch=N [mac=AA:BB:CC:DD:EE:FF] [rate=1-100] [sc=8-64]  ·  csi tx ch=N [rate=1-100] [id=N]  ·  "
                       "csi mesh ch=N id=N [rate=1-100] [slot=I slots=N] [report=50-1000]  ·  csi stop");
        return;
    }
    if (!strcmp(verb, "stop")) {
        if (s_role != ROLE_CSI) { send_line("csi not running"); return; }
        set_role(s_csi_prev == ROLE_CSI ? ROLE_WIFI : s_csi_prev);
        return;
    }
    int m = !strcmp(verb, "start") ? CSI_RX : !strcmp(verb, "tx") ? CSI_TX : !strcmp(verb, "mesh") ? CSI_MESH : -1;
    if (m < 0) { send_line("WD:ERR reason=csi_usage"); return; }
    csi_cfg_t c = { .mode = (csi_mode_t)m, .rate = m == CSI_RX ? 30 : 50, .sc = 32, .slots = 1, .report_ms = 100 };
    int slot = -1;
    for (const char *p = arg + strlen(verb); *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (!strncmp(p, "ch=", 3)) {
            long v = strtol(p + 3, NULL, 10);
            if (v <= 0 || v > 255 || !channel_supported((uint8_t)v)) { send_line("WD:ERR reason=csi_bad_channel"); return; }
            c.ch = (uint8_t)v;
        } else if (!strncmp(p, "mac=", 4)) {
            if (!parse_mac(p + 4, c.mac)) { send_line("WD:ERR reason=csi_bad_mac"); return; }
            c.has_mac = true;
        } else if (!strncmp(p, "rate=", 5)) {
            long v = strtol(p + 5, NULL, 10);
            c.rate = (uint8_t)(v < 1 ? 1 : v > 100 ? 100 : v);
        } else if (!strncmp(p, "sc=", 3)) {
            long v = strtol(p + 3, NULL, 10);
            c.sc = (uint8_t)(v < 8 ? 8 : v > CSI_SC_MAX ? CSI_SC_MAX : v);
        } else if (!strncmp(p, "id=", 3)) {
            long v = strtol(p + 3, NULL, 10);
            if (v < 0 || v > 255) { send_line("WD:ERR reason=csi_bad_id"); return; }
            c.id = (uint8_t)v;
        } else if (!strncmp(p, "slots=", 6)) {
            long v = strtol(p + 6, NULL, 10);
            c.slots = (uint8_t)(v < 1 ? 1 : v > 16 ? 16 : v);
        } else if (!strncmp(p, "slot=", 5)) {
            slot = (int)strtol(p + 5, NULL, 10);
        } else if (!strncmp(p, "via=", 4)) {
            if (!strncmp(p + 4, "raw", 3)) c.raw = true;
            else if (!strncmp(p + 4, "espnow", 6)) c.raw = false;
            else { send_line("WD:ERR reason=csi_bad_via"); return; }
        } else if (!strncmp(p, "report=", 7)) {
            long v = strtol(p + 7, NULL, 10);
            c.report_ms = (uint16_t)(v < 50 ? 50 : v > 1000 ? 1000 : v);
        }
        p += strcspn(p, " ");
    }
    if (!c.ch) { send_line("WD:ERR reason=csi_no_channel"); return; }
    if (c.mode != CSI_MESH) c.slots = 1;   // slots time off slot 0's frames, which only a listening (mesh) board hears
    if (slot < 0) slot = c.id % c.slots;
    if (slot >= c.slots) { send_line("WD:ERR reason=csi_bad_slot"); return; }
    c.slot = (uint8_t)slot;
    if (s_ota_active) { send_line("WD:ERR reason=busy"); return; }
    s_csi_cfg = c;
    if (s_role != ROLE_CSI) {
        s_csi_prev = s_role;
        set_role(ROLE_CSI);
    } else if (s_csi_task || s_csi_running) {   // already on: restart with the new settings
        send_line("WD:MODE mode=csi");
        s_switch_pending = true;
        stream_stop();
    } else {
        set_role(ROLE_CSI);                      // the role was paused (`stop`): run it again
    }
#endif
}

static void reboot_task(void *arg) {
    (void)arg;
    wait_role_tasks(2000);           // queued records go out first
    vTaskDelay(pdMS_TO_TICKS(400));  // let the last lines go out
    esp_restart();
}

static void cmd_reboot(void) {
    send_line("rebooting");
    s_rebooting = true;
    s_switch_pending = false;
    stream_stop();
    if (xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL) != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    }
}

static SemaphoreHandle_t s_cmd_lock;   // commands from BLE and USB run one at a time

static void exec_command(char *p) {
    char word[16];
    size_t wlen = strcspn(p, " ");
    if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
    memcpy(word, p, wlen);
    word[wlen] = '\0';
    const char *rest = p + strcspn(p, " ");
    while (*rest == ' ') rest++;

    if (!strcmp(word, "ota")) {
        if (!strncmp(rest, "begin ", 6)) ota_begin(rest + 6);
        else if (!strcmp(rest, "end")) { if (s_ota_active) s_ota_end_req = true; }
        else if (!strcmp(rest, "abort")) { if (s_ota_active) s_ota_abort_req = true; }
    }
    else if (!strcmp(word, "help"))     cmd_help();
    else if (!strcmp(word, "info"))     cmd_info();
    else if (!strcmp(word, "status"))   cmd_status();
    else if (!strcmp(word, "caps"))     send_caps_line();
    else if (!strcmp(word, "ch"))       cmd_channels(rest);
    else if (!strcmp(word, "reboot"))   cmd_reboot();
    else if (!strcmp(word, "link"))     cmd_link();
    else if (!strcmp(word, "drop"))     cmd_drop();
    else if (!strcmp(word, "hb") || !strcmp(word, "client")) {}   // sent by app builds from 2026-10-22: ignore quietly
    else if (!strcmp(word, "bye")) { if (s_cmd_src == SINK_BLE) s_release_req = true; }   // the app lets go on purpose (1.7.3+, silent)
    else if (!strcmp(word, "iam")) {   // "iam a|i <nickname>": who holds this link, for the status beacon (silent)
        if ((rest[0] == 'a' || rest[0] == 'i') && rest[1] == ' ') {
            s_holder_ios = rest[0] == 'i';
            size_t h = strlen(rest + 2);
            if (h > HOLDER_MAX) { h = HOLDER_MAX; while (h && ((uint8_t)rest[2 + h] & 0xC0) == 0x80) h--; }   // whole UTF-8 characters only
            memcpy(s_holder, rest + 2, h); s_holder[h] = '\0';
            if (host_linked() && s_link_ready) ble_npl_callout_reset(&s_beacon_tick, 0);
        }
    }
    else if (!strcmp(word, "led"))      cmd_led(rest);
    else if (!strcmp(word, "board"))    cmd_board(rest);
    else if (!strcmp(word, "screen"))   cmd_screen();
    else if (!strcmp(word, "coproc"))   cmd_coproc(rest);
    else if (!strcmp(word, "csi"))      cmd_csi(rest);
    else if (!strcmp(word, "sd"))       cmd_sd(rest, s_cmd_src, true);
    else if (!strcmp(word, "time")) {   // "time <unix seconds>": the app sets the board's UTC clock
        long long t = atoll(rest);
        if (t) clock_set((time_t)t, "host");
        else send_line_to(s_cmd_src, "WD:TIME now=%lld set=%d", (long long)time(NULL), s_clock_set ? 1 : 0);
    }
#if CONFIG_WARDRIVEESP_BENCH_TOOLS
    else if (!strcmp(word, "pinsense")) cmd_pinsense(rest);
    else if (!strcmp(word, "benchbt")) {
        s_bench_keep_bt = !strcmp(rest, "on");
        bt_power_kick();
        send_line("bench: Bluetooth %s during USB sessions", s_bench_keep_bt ? "kept on" : "off");
    }
#endif
    else if (!strcmp(word, "stop"))     stream_stop();
    else if (!strcmp(word, "start"))    stream_start();
    else if (!strcmp(word, "bt")) {
        if (!strcmp(rest, "on"))       set_role(ROLE_BT);
        else if (!strcmp(rest, "off")) set_role(ROLE_WIFI);
        else send_line("mode %s  ·  bt on | bt off",
                       s_role == ROLE_BT ? "Bluetooth" : s_role == ROLE_CAP ? "Capture" : "Wi-Fi");
    }
    else if (!strcmp(word, "cap")) {
        if (!strcmp(rest, "on"))       set_role(ROLE_CAP);
        else if (!strcmp(rest, "off")) set_role(ROLE_WIFI);
        else send_line("mode %s  ·  cap on | cap off",
                       s_role == ROLE_CAP ? "Capture" : s_role == ROLE_BT ? "Bluetooth" : "Wi-Fi");
    }
    else if (!strcmp(word, "wdstream")) {
        if (strstr(rest, "stop")) { stream_stop(); return; }
        const char *ch_arg = strstr(rest, "-ch ");
        if (ch_arg && !apply_channel_list(ch_arg + 4)) send_line("WD:ERR reason=no_supported_channels");
        if (strstr(rest, "-promisc") || strstr(rest, "-cap")) { set_role(ROLE_CAP); return; }
        if (strstr(rest, "-bt"))   { set_role(ROLE_BT);   return; }
        if (strstr(rest, "-wifi")) { set_role(ROLE_WIFI); return; }
        stream_start();
    }
    else { send_line("unknown command '%s'", word); cmd_help(); }
}

static void run_command(const uint8_t *text, size_t len, uint8_t src) {
    char cmd[CMD_MAX + 1];
    size_t n = len < CMD_MAX ? len : CMD_MAX;
    memcpy(cmd, text, n);
    cmd[n] = '\0';
    ESP_LOGI(TAG, "cmd: '%s'", cmd);

    char *p = cmd;
    while (*p == ' ') p++;
    size_t plen = strlen(p);
    while (plen && (p[plen - 1] == ' ' || p[plen - 1] == '\r' || p[plen - 1] == '\n')) p[--plen] = '\0';
    if (!*p) return;
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    s_cmd_src = src;
    heap_check("before command");
    exec_command(p);
    heap_check("after command");
    xSemaphoreGive(s_cmd_lock);
}

static void handle_command(const uint8_t *data, uint16_t len) {
    const uint8_t *text = data;
    uint16_t text_len = len;
    if (len >= FR_HEADER && data[0] == FR_MAGIC_0 && data[1] == FR_MAGIC_1) {
        uint16_t plen = (uint16_t)(data[10] | (data[11] << 8));
        if (FR_HEADER + (size_t)plen <= len) {
            s_reply_id = (uint32_t)data[6] | ((uint32_t)data[7] << 8) | ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
            if (data[3] == FR_CMD) {
                text = data + FR_HEADER;
                text_len = plen;
                send_ack();
            }
        }
    }
    run_command(text, text_len, SINK_BLE);
}

// ─── USB transport (PROTOCOL.md "USB") ─────────────────────────────────────
// One task serves both ports. A port shows ESP-IDF logs until `wdusb hello`, then carries only protocol lines
// until `wdusb bye`, 60 s of silence, or (native USB) unplug / a host that stops reading.
#define USB_IDLE_MS      60000
#define USB_STALL_MS     5000
#define USB_LOST_MS      1000
#define USB_RAW_IDLE_MS  10000

typedef struct {
    uint8_t  sink;
    bool     up;               // driver installed
    bool     overflow;         // current line is too long; dropped at its newline
    size_t   len;
    char     line[CMD_MAX + 1];
    uint32_t last_rx_ms;
    uint32_t lost_ms;          // first moment the host looked gone, 0 = present
    uint32_t raw_left;         // firmware image bytes still due after WD:OTA ready
} usb_port_t;

static usb_port_t s_usj  = { .sink = SINK_USB };
static usb_port_t s_uart = { .sink = SINK_UART };
static volatile uint32_t s_usj_fail_ms = 0;   // first failed write of the current stall, 0 = writes go through
static TaskHandle_t s_usb_task = NULL;
static vprintf_like_t s_log_orig = NULL;
static SemaphoreHandle_t s_log_lock;
static char s_log_buf[256];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Called under s_send_lock. Never blocks: a line that doesn't fit the TX buffer is dropped.
static void usb_emit_locked(uint8_t sinks, const char *buf, size_t n) {
    uint8_t s = sinks & s_usb_sessions;
    (void)s; (void)buf; (void)n;
#if USB_JTAG_LINK
    if ((s & SINK_USB) && s_usj.up) {
        if (usb_serial_jtag_write_bytes(buf, n, 0) == (int)n) s_usj_fail_ms = 0;
        else if (!s_usj_fail_ms) s_usj_fail_ms = now_ms() | 1;
    }
#endif
#if UART_LINK
    if ((s & SINK_UART) && s_uart.up) {
        size_t room = 0;
        if (uart_get_tx_buffer_free_size(LINK_UART, &room) == ESP_OK && room >= n) uart_write_bytes(LINK_UART, buf, n);
    }
#endif
}

// Log text stays off a port while it carries a session; the other port keeps getting it.
static int log_vprintf(const char *fmt, va_list ap) {
    uint8_t sess = s_usb_sessions;
    if (!sess) return s_log_orig ? s_log_orig(fmt, ap) : vprintf(fmt, ap);
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || !s_log_lock) return 0;
    if (xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    int n = vsnprintf(s_log_buf, sizeof(s_log_buf), fmt, ap);
    if (n > (int)sizeof(s_log_buf) - 1) n = (int)sizeof(s_log_buf) - 1;
    if (n > 0) {
#if UART_LINK
        if (!(sess & SINK_UART) && s_uart.up) uart_write_bytes(LINK_UART, s_log_buf, (size_t)n);
#endif
#if USB_JTAG_LINK
        if (!(sess & SINK_USB) && s_usj.up) usb_serial_jtag_write_bytes(s_log_buf, (size_t)n, 0);
#endif
    }
    xSemaphoreGive(s_log_lock);
    return n;
}

// ─── microSD offline runs (sdrun.c) ────────────────────────────────────────
// Koko V8 / AWOK Dual C5: when the last host lets go and a card is mounted, the Wi-Fi scan carries on and every
// sighting goes to /runs/run_<n>.csv (WiGLE CSV). The next link closes the file. No card: nothing here runs.

static void clock_set(time_t t, const char *from) {
    if (t < 1700000000) return;
    time_t now = time(NULL);
    if (s_clock_set && now - t < 2 && t - now < 2) return;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    if (!s_clock_set) ESP_LOGI(TAG, "clock set from %s: %lld", from, (long long)t);
    s_clock_set = true;
}

static void sd_note_ap(const wifi_ap_record_t *ap, size_t ssid_len) {
    sdrun_ap_t r = { .rssi = ap->rssi, .ch = ap->primary, .auth = auth_name(ap->authmode) };
    memcpy(r.bssid, ap->bssid, 6);
    memcpy(r.ssid, ap->ssid, ssid_len);
    r.ssid[ssid_len] = '\0';
    gps_where(&r);
    sdrun_ap(&r);
}

/// The last host let go: with a card in, a new offline run starts and the Wi-Fi scan carries on into it.
static void offline_begin(void) {
    if (s_rebooting || s_ota_active || !sdrun_start()) return;
    ESP_LOGI(TAG, "no host linked: offline run to the microSD card");
    if (s_scan_task || s_bt_task || s_cap_task || s_csi_task) s_switch_pending = true;   // the ending task restarts it
    else stream_start();
}

/// A host linked again: the run closes and the scan it was feeding becomes this host's.
static void offline_end(void) {
    if (!sdrun_active()) return;
    sdrun_stop();
    ESP_LOGI(TAG, "host linked: offline run %s closing (%lu rows)", sdrun_file(), (unsigned long)sdrun_rows());
}

// `sd` replies: to a session as protocol lines, or straight onto a port that has no session (the bench console, where
// the logs are). Called under s_cmd_lock, so the buffer is shared; taken for one command and given back after it.
#define SD_CHUNK 768
static char *s_sd_line;   // a reply line: up to SD_CHUNK bytes as base64, plus the fields
#define SD_LINE  (SD_CHUNK * 4 / 3 + 160)

static void sd_reply(uint8_t sink, bool session, const char *fmt, ...) {
    if (!s_sd_line && !(s_sd_line = malloc(SD_LINE))) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_sd_line, SD_LINE - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > SD_LINE - 2) n = SD_LINE - 2;
    s_sd_line[n++] = '\n';
    if (session) {
        xSemaphoreTake(s_send_lock, portMAX_DELAY);
        emit_locked(sink, s_sd_line, (size_t)n);
        xSemaphoreGive(s_send_lock);
        return;
    }
#if UART_LINK
    if (sink == SINK_UART && s_uart.up) uart_write_bytes(LINK_UART, s_sd_line, (size_t)n);
#endif
#if USB_JTAG_LINK
    if (sink == SINK_USB && s_usj.up) usb_serial_jtag_write_bytes(s_sd_line, (size_t)n, pdMS_TO_TICKS(100));
#endif
}

static void b64(char *out, const uint8_t *in, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

typedef struct { uint8_t sink; bool session; } sd_ctx_t;
static void sd_list_one(const char *name, uint32_t size, void *ctx) {
    const sd_ctx_t *c = ctx;
    bool open = sdrun_active() && !strcasecmp(name, sdrun_file());   // still being written: not for taking yet
    sd_reply(c->sink, c->session, "WD:SD file name=%s size=%lu open=%d", name, (unsigned long)size, open ? 1 : 0);
}

/// `sd [status]`, `sd mount`, `sd list`, `sd read <file> <offset> <len>` (base64, up to 768 bytes), `sd del <file>`.
static void sd_command(const char *arg, uint8_t sink, bool session);

static void cmd_sd(const char *arg, uint8_t sink, bool session) {
    sd_command(arg, sink, session);
    free(s_sd_line);   // given back: offline the heap is tight, and `sd` commands come seldom
    s_sd_line = NULL;
}

static void sd_command(const char *arg, uint8_t sink, bool session) {
    while (*arg == ' ') arg++;
    if (!*arg || !strcmp(arg, "status")) {
        if (!sdrun_mounted()) {
            sd_reply(sink, session, "WD:SD mounted=0 slot=%d", lcd_sd_slot(NULL, NULL) ? 1 : 0);
            return;
        }
        uint32_t total_mb, free_mb;
        sdrun_space(&total_mb, &free_mb);
        const char *f = sdrun_file();
        sd_reply(sink, session, "WD:SD mounted=1 total_mb=%lu free_mb=%lu run=%s file=%s rows=%lu dropped=%lu "
                 "remounts=%lu sweeps=%lu clock=%d heap=%lu heap_min=%lu", (unsigned long)total_mb, (unsigned long)free_mb,
                 sdrun_active() ? "open" : f[0] ? "closed" : "none", f[0] ? f : "-", (unsigned long)sdrun_rows(),
                 (unsigned long)sdrun_dropped(), (unsigned long)sdrun_remounts(), (unsigned long)s_sweeps, s_clock_set ? 1 : 0,
                 (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
        return;
    }
    if (!strcmp(arg, "test")) {
        sd_reply(sink, session, "WD:SD test=%s", sdrun_test());
        return;
    }
    if (!strcmp(arg, "mount")) {
        sd_reply(sink, session, "WD:SD mount=%s", sdrun_mounted() ? "already" : sdrun_remount() ? "trying" : "no_slot");
        return;
    }
    if (!strcmp(arg, "list")) {
        sd_ctx_t c = { sink, session };
        int n = sdrun_list(sd_list_one, &c);
        if (n < 0) sd_reply(sink, session, "WD:SD err reason=not_mounted");
        else sd_reply(sink, session, "WD:SD list n=%d", n);
        return;
    }
    char name[24] = "";
    if (!strncmp(arg, "read ", 5)) {
        unsigned long off = 0, len = 0;
        if (sscanf(arg + 5, "%23s %lu %lu", name, &off, &len) != 3 || !len) {
            sd_reply(sink, session, "WD:SD err reason=usage  (sd read <file> <offset> <len>)");
            return;
        }
        if (len > SD_CHUNK) len = SD_CHUNK;
        uint8_t *raw = malloc(SD_CHUNK);
        char *enc = malloc(SD_CHUNK * 4 / 3 + 4);
        uint32_t size = 0;
        int n = raw && enc ? sdrun_read(name, (uint32_t)off, raw, len, &size) : -2;
        if (n >= 0) {
            sdx_read(name, (uint32_t)off, n, size);
            b64(enc, raw, (size_t)n);
            sd_reply(sink, session, "WD:SD data name=%s off=%lu len=%d size=%lu crc=%08lx b64=%s", name, off, n,
                     (unsigned long)size, (unsigned long)esp_rom_crc32_le(0, raw, (uint32_t)n), enc);
        } else {
            sd_reply(sink, session, "WD:SD err reason=%s name=%s", n == -2 ? "no_memory" : sdrun_mounted() ? "no_file" :
                     "not_mounted", name);
        }
        free(raw);
        free(enc);
        return;
    }
    if (!strncmp(arg, "del ", 4)) {
        if (sscanf(arg + 4, "%23s", name) != 1) { sd_reply(sink, session, "WD:SD err reason=usage  (sd del <file>)"); return; }
        int r = sdrun_delete(name);
        if (r == 0) sdx_deleted(name);
        if (r == 0) sd_reply(sink, session, "WD:SD del name=%s ok=1", name);
        else sd_reply(sink, session, "WD:SD del name=%s ok=0 reason=%s", name, r == -2 ? "open_run" : "no_file");
        return;
    }
    sd_reply(sink, session, "sd status | mount | test | list | read <file> <offset> <len> | del <file>");
}

static usb_port_t *usb_port(uint8_t sink) { return sink == SINK_USB ? &s_usj : &s_uart; }

// Called from ota_begin (in the USB task) once the board is ready for the image.
static void usb_arm_raw(uint8_t sink, uint32_t size) { usb_port(sink)->raw_left = size; }

// Called under s_cmd_lock.
static void usb_session_end(usb_port_t *p, const char *reason) {
    if (!(s_usb_sessions & p->sink)) return;
    if (reason) send_line_to(p->sink, "WD:USB bye reason=%s", reason);
    s_usb_sessions &= (uint8_t)~p->sink;
    s_cp_sinks &= (uint8_t)~p->sink;
    if (p->sink == SINK_USB) s_usj_fail_ms = 0;
    if (s_ota_active && s_ota_src == p->sink) s_ota_abort_req = true;
    p->raw_left = 0;
    if (!host_linked()) {
        s_switch_pending = false;
        stream_stop();
        s_role = ROLE_WIFI;
        plan_reset_to_all();
        s_holder[0] = '\0'; s_holder_ios = false;
        s_link_end = reason && !strcmp(reason, "host") ? LINK_END_RELEASED : LINK_END_LOST;
        offline_begin();
    }
    if (s_link_ready) ble_npl_callout_reset(&s_link_switch, 0);   // connectable again once no USB host is left
    bt_power_kick();
    ESP_LOGI(TAG, "%s session closed (%s)", sink_name(p->sink), reason ? reason : "host gone");
}

// Called under s_cmd_lock.
static void usb_hello(usb_port_t *p) {
    if (!(s_usb_sessions & p->sink)) {
        ESP_LOGI(TAG, "%s session open", sink_name(p->sink));
        if (!host_linked()) {
            s_role = ROLE_WIFI;
            s_switch_pending = false;
            plan_reset_to_all();
        }
        if (p->sink == SINK_USB) s_usj_fail_ms = 0;
        p->lost_ms = 0;
        offline_end();
        s_usb_sessions |= p->sink;
        if (s_link_ready) ble_npl_callout_reset(&s_link_switch, 0);   // USB-only: Bluetooth steps aside
        bt_power_kick();
        led_storm();
    }
    host_joined(p->sink);
}

// ─── Biscuit co-processor (PROTOCOL.md "Co-processor (WDCP)", 1.6.7) ──────
// On a Biscuit Pro / Ultra the C5's UART0 pins (TX GPIO 11 / RX GPIO 12, the console and UART link above) are wired
// to the board's second chip, an ESP32 WROOM that holds the battery gauge. With the WardriveESP co-processor flashed
// there, the WROOM says `WDCP hello` and then sends the gauge every 30 s. Listening costs nothing (UART0 RX is up on
// every C5 build anyway), and nothing is ever sent to it until a valid hello was heard, so other C5 boards never see
// a WDCP line. A hello is also the board's fingerprint: only a Biscuit two-chip board has a WROOM on these pins.
#if CONFIG_IDF_TARGET_ESP32C5 && UART_LINK
#define COPROC 1
#define COPROC_PING_MS   30000
#define COPROC_STALE_MS  120000
static volatile bool     s_cp_present;
static volatile uint32_t s_cp_last_rx;    // last line from the co-processor
static uint32_t s_cp_last_ping;
static char s_cp_fw[16];
// Its second radio (co-processor 0.2.0+, hello `scan=1`): Bluetooth Classic or Wi-Fi 2.4 GHz, results forwarded only to
// hosts that asked (`coproc`), so an app that predates it never stores Bluetooth Classic devices as BLE.
static volatile bool s_cp_scan;
static char s_cp_mode[8] = "off";
static char s_cp_state[8] = "boot";
static volatile uint32_t s_cp_n, s_cp_total, s_cp_dropped;
static int s_cp_run_sent = -1;            // last `run` sent, -1 = none since its hello

static bool coproc_mode_valid(const char *m) { return !strcmp(m, "off") || !strcmp(m, "btc") || !strcmp(m, "wifi"); }
static const char *coproc_mode_label(const char *m) {
    return !strcmp(m, "btc") ? "Bluetooth Classic" : !strcmp(m, "wifi") ? "Wi-Fi 2.4 GHz" : "off";
}

/// WD:CAPS's `coproc=` from the current state. True when it changed. Called under s_cmd_lock.
static bool coproc_caps_update(void) {
    char c[sizeof(s_cp_caps)] = "";
    if (s_cp_present && s_cp_scan) snprintf(c, sizeof(c), "coproc=%s ", s_cp_mode);
    if (!strcmp(c, s_cp_caps)) return false;
    strlcpy(s_cp_caps, c, sizeof(s_cp_caps));
    return true;
}

/// Hosts that asked for co-processor results and are still linked.
static uint8_t coproc_hosts(void) {
    uint8_t live = s_usb_sessions | ((s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_notify_enabled) ? SINK_BLE : 0);
    return s_cp_sinks & live;
}

static void coproc_status_to(uint8_t sinks) {
    if (!sinks) return;
    send_line_to(sinks, "WD:COPROC mode=%s state=%s n=%lu total=%lu dropped=%lu fw=%s", s_cp_mode, s_cp_state,
                 (unsigned long)s_cp_n, (unsigned long)s_cp_total, (unsigned long)s_cp_dropped, s_cp_fw);
}

static void coproc_send(const char *what) {
    if (!s_uart.up) return;
    char buf[48];   // leading newline: ends any log line in flight on the shared wire first
    int n = snprintf(buf, sizeof(buf), "\r\nWDCP %s\r\n", what);
    if (n > 0 && n < (int)sizeof(buf)) uart_write_bytes(LINK_UART, buf, (size_t)n);
}

/// The value of `key=` in a line of space-separated key=value pairs, copied into out ("" when absent).
static void coproc_kv(const char *s, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    out[0] = '\0';
    for (const char *p = s; (p = strstr(p, key)) != NULL; p += kl) {
        if ((p != s && p[-1] != ' ') || p[kl] != '=') continue;
        const char *v = p + kl + 1;
        size_t n = strcspn(v, " \r\n");
        if (n >= cap) n = cap - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return;
    }
}

static void coproc_hello(const char *args) {
    char proto[8], board[24], fw[16], gauge[4], scan[4], mode[8];
    coproc_kv(args, "proto", proto, sizeof(proto));
    coproc_kv(args, "board", board, sizeof(board));
    coproc_kv(args, "fw", fw, sizeof(fw));
    coproc_kv(args, "gauge", gauge, sizeof(gauge));
    coproc_kv(args, "scan", scan, sizeof(scan));
    coproc_kv(args, "mode", mode, sizeof(mode));
    board_t nb = !strcmp(board, "biscuit_pro") ? BOARD_BISCUIT_PRO : !strcmp(board, "biscuit_ultra") ? BOARD_BISCUIT_ULTRA :
                 !strcmp(board, "biscuit_dual") ? BOARD_BISCUIT_DUAL : BOARD_NONE;
    if (atoi(proto) < 1 || nb == BOARD_NONE || !fw[0]) return;   // not a hello we understand: stay silent
    bool with_gauge = !strcmp(gauge, "1");
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    bool news = !s_cp_present || s_board != nb || (with_gauge && !s_has_batt) || strcmp(fw, s_cp_fw);
    // A board identified some other way (a Crumb's gauge, the Poltergeist's screen) keeps that identity.
    if (news && (s_board == BOARD_NONE || (s_board >= BOARD_BISCUIT_PRO && s_board <= BOARD_BISCUIT_DUAL))) {
        s_cp_present = true;
        strlcpy(s_cp_fw, fw, sizeof(s_cp_fw));
        s_board = nb;
        board_caps_set(&BOARDS[nb]);
        if (with_gauge) s_has_batt = true;
        led_apply(BOARDS[nb].led_gpio, BOARDS[nb].pixels);   // a new model takes or drops its ring now (led= in WD:CAPS)
        ESP_LOGI(TAG, "co-processor %s on UART0: board %s, battery gauge %s", fw, BOARDS[nb].name, with_gauge ? "yes" : "no");
    } else {
        news = false;
    }
    if (s_cp_present) {
        s_cp_scan = !strcmp(scan, "1") && coproc_mode_valid(mode);
        if (s_cp_scan) strlcpy(s_cp_mode, mode, sizeof(s_cp_mode));
        if (coproc_caps_update()) {
            ESP_LOGI(TAG, "co-processor radio: %s", s_cp_scan ? coproc_mode_label(s_cp_mode) : "none");
            news = true;
        }
    }
    if (news) send_caps_line();   // linked hosts get the board, batt=1 and coproc= now, not at their next link
    xSemaphoreGive(s_cmd_lock);
    if (!s_cp_present) return;
    s_cp_last_rx = s_cp_last_ping = now_ms();
    s_cp_run_sent = -1;     // it just (re)started: tell it again whether to scan (coproc_tick)
    coproc_send("ping");    // answered: the co-processor stops repeating its hello
    coproc_send("batt?");
    if (s_cp_scan) { strlcpy(s_cp_state, "boot", sizeof(s_cp_state)); coproc_send("stat?"); }
}

static void hex12_mac(const char *h, char out[18]) {   // "AABBCCDDEEFF" -> "AA:BB:CC:DD:EE:FF"
    for (int i = 0; i < 6; i++) {
        out[3 * i] = (char)toupper((unsigned char)h[2 * i]);
        out[3 * i + 1] = (char)toupper((unsigned char)h[2 * i + 1]);
        out[3 * i + 2] = i < 5 ? ':' : '\0';
    }
}

/// Hex digits only; exactly n_exact of them, or (n_exact 0) any even count including none.
static bool all_hex(const char *s, size_t n_exact) {
    size_t n = strlen(s);
    if (n_exact ? n != n_exact : (n % 2) != 0) return false;
    for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

static void lower_ascii(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/// `WDCP bt <mac> <rssi> <cod> [name hex]` -> WD:BT flagged classic, to the hosts that asked.
static void coproc_bt(const char *a) {
    uint8_t sinks = coproc_hosts();
    if (!sinks) return;
    char mac[16] = "", cod[10] = "", name[80] = "", m[18];
    int rssi = 0;
    if (sscanf(a, "%15s %d %9s %79s", mac, &rssi, cod, name) < 3 || !all_hex(mac, 12) || !all_hex(cod, 6)) return;
    if (!all_hex(name, 0)) name[0] = '\0';
    lower_ascii(name);
    lower_ascii(cod);
    hex12_mac(mac, m);
    send_line_to(sinks, "WD:BT ts=%lu mac=%s type=public rssi=%d name_hex=%s mfg= conn=0 kind=classic cod=%s src=coproc",
                 (unsigned long)now_ms(), m, rssi, name, cod);
}

/// `WDCP ap <bssid> <ch> <rssi> <auth> <hidden> [ssid hex]` -> WD:AP from the co-processor, to the hosts that asked.
static void coproc_ap(const char *a) {
    uint8_t sinks = coproc_hosts();
    if (!sinks) return;
    char bssid[16] = "", auth[12] = "", ssid[80] = "", b[18];
    int ch = 0, rssi = 0, hidden = 0;
    if (sscanf(a, "%15s %d %d %11s %d %79s", bssid, &ch, &rssi, auth, &hidden, ssid) < 5 || !all_hex(bssid, 12) ||
        ch < 1 || ch > 14) return;
    if (hidden || !all_hex(ssid, 0)) ssid[0] = '\0';
    lower_ascii(ssid);
    for (char *c = auth; *c; c++) if (!isalnum((unsigned char)*c) && *c != '_') { strlcpy(auth, "UNKNOWN", sizeof(auth)); break; }
    hex12_mac(bssid, b);
    send_line_to(sinks, "WD:AP ts=%lu bssid=%s ssid_hex=%s rssi=%d ch=%d auth=%s hidden=%d src=coproc",
                 (unsigned long)now_ms(), b, ssid, rssi, ch, auth, hidden ? 1 : 0);
}

/// The co-processor's mode as it reports it (stat, mode reply). Updates WD:CAPS when it changed.
static void coproc_mode_seen(const char *mode, const char *state) {
    if (!coproc_mode_valid(mode)) return;
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    strlcpy(s_cp_mode, mode, sizeof(s_cp_mode));
    if (state) strlcpy(s_cp_state, state, sizeof(s_cp_state));
    if (coproc_caps_update()) send_caps_line();
    xSemaphoreGive(s_cmd_lock);
}

/// A line from UART0: true when it was the co-processor's (consumed), false for anything else.
static bool coproc_line(const char *line) {
    const char *w = strstr(line, "WDCP ");   // our own log text can run into it on the wire
    if (!w) return false;
    const char *c = w + 5;
    while (*c == ' ') c++;
    if (!strncmp(c, "hello ", 6)) { coproc_hello(c + 6); return true; }
    if (!s_cp_present) return true;          // nothing else counts before a valid hello
    s_cp_last_rx = now_ms();
    if (!strncmp(c, "batt ", 5)) {
        int pct = -1, mv = 0, rate = 0;
        int got = sscanf(c + 5, "%d %d %d", &pct, &mv, &rate);
        if (got >= 2) {
            s_batt_pct = (pct >= 0 && pct <= 100) ? pct : -1;
            s_batt_mv = pct >= 0 ? mv : 0;
            s_batt_rate = rate;
            s_batt_rate_ok = got == 3 && pct >= 0;   // co-processor 0.2.0+ adds the gauge's charge rate
        }
    } else if (!strncmp(c, "bt ", 3)) {
        coproc_bt(c + 3);
    } else if (!strncmp(c, "ap ", 3)) {
        coproc_ap(c + 3);
    } else if (!strncmp(c, "stat ", 5)) {
        char mode[8] = "", state[8] = "";
        unsigned long n = 0, total = 0, dropped = 0;
        if (sscanf(c + 5, "%7s %7s %lu %lu %lu", mode, state, &n, &total, &dropped) == 5) {
            s_cp_n = n; s_cp_total = total; s_cp_dropped = dropped;
            coproc_mode_seen(mode, state);
            coproc_status_to(coproc_hosts());
        }
    } else if (!strncmp(c, "mode ", 5)) {   // `mode <m>` (no change) or `mode <m> restart` (it restarts into it)
        char mode[8] = "", what[12] = "";
        if (sscanf(c + 5, "%7s %11s", mode, what) >= 1) {
            bool restart = !strcmp(what, "restart");
            coproc_mode_seen(mode, restart ? "boot" : NULL);
            if (restart) { s_cp_n = 0; s_cp_run_sent = -1; }
            coproc_status_to(coproc_hosts());
        }
    } else if (!strncmp(c, "err ", 4)) {
        uint8_t sinks = coproc_hosts();
        if (sinks) send_line_to(sinks, "coproc: refused (%s)", c + 4);
    }
    return true;   // pong and later verbs: nothing to do
}

/// Scan only while it is useful: a scan-capable co-processor in a radio mode, a linked host that asked for its
/// results, and the board itself scanning. Sent on every change and renewed with each ping (the co-processor stops
/// by itself when the renewals stop).
static void coproc_run_update(bool renew) {
    if (!s_cp_present || !s_cp_scan) return;
    int want = (strcmp(s_cp_mode, "off") && coproc_hosts() && (s_streaming || s_bt_running || s_cap_running)) ? 1 : 0;
    if (want == s_cp_run_sent && !(renew && want)) return;
    s_cp_run_sent = want;
    coproc_send(want ? "run 1" : "run 0");
}

/// From the health task, once a second: keep the co-processor answering, and forget a battery level it stopped sending.
static void coproc_tick(uint32_t now) {
    if (!s_cp_present) return;
    bool renew = false;
    if (now - s_cp_last_ping >= COPROC_PING_MS) { s_cp_last_ping = now; coproc_send("ping"); renew = true; }
    coproc_run_update(renew);
    if (s_batt_pct >= 0 && now - s_cp_last_rx > COPROC_STALE_MS) {
        s_batt_pct = -1;
        s_batt_rate_ok = false;
        ESP_LOGW(TAG, "co-processor silent for %lu s, battery unknown", (unsigned long)(COPROC_STALE_MS / 1000));
    }
}

/// `coproc`: the co-processor radio's state, and from now on its results for this host. `coproc mode btc|wifi|off`
/// changes the mode (saved on the co-processor, which restarts into it). Called under s_cmd_lock.
static void cmd_coproc(const char *arg) {
    if (!s_cp_present || !s_cp_scan) {
        send_line_to(s_cmd_src, "no co-processor radio on this board%s", s_cp_present ? " (co-processor firmware too old)" : "");
        return;
    }
    s_cp_sinks |= s_cmd_src;
    if (!strncmp(arg, "mode", 4)) {
        const char *m = arg + 4;
        while (*m == ' ') m++;
        if (!coproc_mode_valid(m)) { send_line_to(s_cmd_src, "coproc mode btc | wifi | off  (now %s)", s_cp_mode); return; }
        char buf[16];
        snprintf(buf, sizeof(buf), "mode %s", m);
        coproc_send(buf);
        send_line_to(s_cmd_src, "coproc mode %s requested", m);
        return;
    }
    coproc_status_to(s_cmd_src);
    coproc_send("stat?");
}

static void coproc_info(void) {
    if (!s_cp_present) return;
    if (!s_cp_scan) { send_line("co-processor %s  ·  battery only", s_cp_fw); return; }
    send_line("co-processor %s  ·  %s  ·  %s  ·  %lu last window  ·  %lu total  ·  %lu dropped", s_cp_fw,
              coproc_mode_label(s_cp_mode), s_cp_state, (unsigned long)s_cp_n, (unsigned long)s_cp_total,
              (unsigned long)s_cp_dropped);
}
#else
#define COPROC 0
static void cmd_coproc(const char *arg) { (void)arg; send_line_to(s_cmd_src, "no co-processor radio on this board"); }
static void coproc_info(void) {}
#endif

static void usb_line(usb_port_t *p) {
    p->line[p->len] = '\0';
    char *s = p->line;
#if COPROC
    if (p->sink == SINK_UART && coproc_line(s)) return;
#endif
    while (*s == ' ') s++;
    if (!strncmp(s, "wdusb", 5) && (s[5] == '\0' || s[5] == ' ')) {
        const char *arg = s + 5;
        while (*arg == ' ') arg++;
        bool in_session = s_usb_sessions & p->sink;
        xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
        if (!strncmp(arg, "hello", 5)) usb_hello(p);
        else if (in_session && !strncmp(arg, "ping", 4)) send_line_to(p->sink, "WD:USB pong");
        else if (in_session && !strncmp(arg, "bye", 3)) usb_session_end(p, "host");
        xSemaphoreGive(s_cmd_lock);
        return;
    }
    if (s_usb_sessions & p->sink) run_command((const uint8_t *)s, strlen(s), p->sink);
    else if (!strncmp(s, "sd", 2) && (s[2] == '\0' || s[2] == ' ')) {   // bench: the card without opening a session
        xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
        cmd_sd(s + 2, p->sink, false);
        xSemaphoreGive(s_cmd_lock);
    }
}

static void usb_rx(usb_port_t *p, const uint8_t *buf, size_t n) {
    p->last_rx_ms = now_ms();
    for (size_t i = 0; i < n;) {
        if (p->raw_left) {
            size_t take = n - i < p->raw_left ? n - i : p->raw_left;
            if (s_ota_active && s_ota_src == p->sink && s_ota_buf &&
                xStreamBufferSend(s_ota_buf, buf + i, take, pdMS_TO_TICKS(2000)) != take)
                s_ota_abort_req = true;   // host ignored the window
            p->raw_left -= take;
            i += take;
            continue;
        }
        char c = (char)buf[i++];
        if (c == '\n' || c == '\r') {
            if (p->len && !p->overflow) usb_line(p);
            p->len = 0;
            p->overflow = false;
        } else if (p->len < CMD_MAX) {
            p->line[p->len++] = c;
        } else {
            p->overflow = true;
        }
    }
}

static void usb_watch(usb_port_t *p, uint32_t now, bool gone) {
    if (p->raw_left && now - p->last_rx_ms > USB_RAW_IDLE_MS) {
        p->raw_left = 0;
        if (s_ota_active && s_ota_src == p->sink) s_ota_abort_req = true;
    }
    if (!(s_usb_sessions & p->sink)) return;
    if (!gone) p->lost_ms = 0;
    else if (!p->lost_ms) p->lost_ms = now | 1;
    bool lost = p->lost_ms && now - p->lost_ms > USB_LOST_MS;
    bool idle = now - p->last_rx_ms > USB_IDLE_MS;
    if (!lost && !idle) return;
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    usb_session_end(p, lost ? NULL : "idle");
    xSemaphoreGive(s_cmd_lock);
}

static void usb_task(void *arg) {
    (void)arg;
    static uint8_t buf[512];
    for (;;) {
#if USB_JTAG_LINK
        if (s_usj.up) {
            int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(10));
            if (n > 0) usb_rx(&s_usj, buf, (size_t)n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
#else
        vTaskDelay(pdMS_TO_TICKS(10));
#endif
#if UART_LINK
        size_t avail = 0;
        while (s_uart.up && uart_get_buffered_data_len(LINK_UART, &avail) == ESP_OK && avail) {
            int m = uart_read_bytes(LINK_UART, buf, avail < sizeof(buf) ? avail : sizeof(buf), 0);
            if (m <= 0) break;
            usb_rx(&s_uart, buf, (size_t)m);
        }
#endif
        uint32_t now = now_ms();
#if USB_JTAG_LINK
        uint32_t fail = s_usj_fail_ms;
        usb_watch(&s_usj, now, !usb_serial_jtag_is_connected() || (fail && now - fail > USB_STALL_MS));
#endif
#if UART_LINK
        usb_watch(&s_uart, now, false);
#endif
    }
}

static void usb_link_start(void) {
    s_log_lock = xSemaphoreCreateMutex();
#if USB_JTAG_LINK
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.tx_buffer_size = 4096;
    ucfg.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        s_usj.up = true;
    } else {
        ESP_LOGW(TAG, "USB-Serial-JTAG link unavailable");
    }
#endif
#if UART_LINK
    if (uart_driver_install(LINK_UART, 2048, 4096, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(LINK_UART);
        s_uart.up = true;
    } else {
        ESP_LOGW(TAG, "UART0 link unavailable");
    }
#endif
    if (!s_usj.up && !s_uart.up) return;
    s_log_orig = esp_log_set_vprintf(log_vprintf);
    if (xTaskCreate(usb_task, "wd_usb", 8192, NULL, 4, &s_usb_task) != pdPASS) ESP_LOGE(TAG, "USB task create failed");
}

// Commands written over BLE run right AFTER their write response goes out, not inside the write callback. Run inside,
// a command's ack and output lines were queued ahead of the response and could take the last buffers, so the response
// was never sent and the phone dropped the link after its 30 s GATT timeout (field 2026-10-26: the board ran
// `wdstream start` and streamed its reply, the link dropped 30 s after the write). Host task only, so no lock.
#define CMD_RING 6
static uint8_t  s_cmd_ring[CMD_RING][256];
static uint16_t s_cmd_ring_len[CMD_RING];
static uint8_t  s_cmd_head = 0, s_cmd_tail = 0;   // free-running; head - tail = commands waiting

static void cmd_drain(struct ble_npl_event *ev) {
    (void)ev;
    while (s_cmd_tail != s_cmd_head) {
        uint8_t i = s_cmd_tail % CMD_RING;
        handle_command(s_cmd_ring[i], s_cmd_ring_len[i]);
        s_cmd_tail++;
    }
}

static int on_command_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (s_link_ready && (uint8_t)(s_cmd_head - s_cmd_tail) < CMD_RING) {
        uint8_t i = s_cmd_head % CMD_RING;
        uint16_t got = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_cmd_ring[i], sizeof(s_cmd_ring[i]), &got) != 0) return BLE_ATT_ERR_UNLIKELY;
        s_cmd_ring_len[i] = got;
        s_cmd_head++;
        ble_npl_callout_reset(&s_cmd_drain, 0);
        return 0;
    }
    // Queue full (or no host queue yet): run it now rather than lose it.
    uint8_t buf[256];
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &got) != 0) return BLE_ATT_ERR_UNLIKELY;
    handle_command(buf, got);
    return 0;
}

static int on_tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;
}

static const struct ble_gatt_chr_def CHARACTERISTICS[] = {
    { .uuid = &RX_UUID.u,   .access_cb = on_command_write, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &TX_UUID.u,   .access_cb = on_tx_access,     .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_tx_val_handle },
    { .uuid = &CTRL_UUID.u, .access_cb = on_command_write, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &OTA_UUID.u,  .access_cb = on_ota_write,     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { 0 },
};
static const struct ble_gatt_svc_def SERVICES[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC_UUID.u, .characteristics = CHARACTERISTICS },
    { 0 },
};

// ─── Status beacon (1.6.5) ───────────────────────────────────────────────
// Every phone nearby sees every board and who holds it, so a board busy on another phone is shown as busy instead
// of looking broken. Manufacturer data (company 0xFFFF), little-endian:
//   [0] 'W'   [1] flags: b0 linked, b1 scanning, b2-3 mode (0 Wi-Fi, 1 Bluetooth, 2 capture), b4 USB host,
//                       b5 GPS, b6 updating, b7 holder is an iPhone
//   [2] chip temperature, °C (int8, -128 = unknown)   [3-4] APs last sweep / BT devices last window / capture: APs
//       heard in the last 20-30 s (1.7.1+; before, every AP line sent, so re-sends counted again)
//   [5] firmware major<<4 | minor   [6] patch   [7-8] the address bytes in the board's name
//   [9..] the holder's nickname, UTF-8, up to 18 bytes (linked boards only)
// A free board: SVC_UUID + name + bytes 0-8 (connectable, as before). A linked board: STATUS_UUID + bytes 0-26
// (non-connectable, once a second).
static volatile int8_t s_temp_c = -128;
static uint8_t s_beacon_last[BEACON_FIXED + HOLDER_MAX];
static size_t s_beacon_last_len = 0;
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_tsens;
#endif

static size_t beacon_payload(uint8_t *b, bool with_holder) {
    static int fw_maj = -1, fw_min, fw_pat;
    if (fw_maj < 0 && sscanf(FW_VERSION, "%d.%d.%d", &fw_maj, &fw_min, &fw_pat) != 3) { fw_maj = fw_min = fw_pat = 0; }
    uint32_t n = s_role == ROLE_BT ? s_bt_last : s_role == ROLE_CAP ? apc_live((uint32_t)(esp_timer_get_time() / 1000)) :
                 s_role == ROLE_CSI ? s_csi_pps : s_last_aps;
    if (n > 0xFFFF) n = 0xFFFF;
    bool linked = host_linked();   // a Bluetooth phone or a USB host
    b[0] = 'W';
    b[1] = (linked ? 0x01 : 0) | ((s_streaming || s_bt_running || s_cap_running || s_csi_running) ? 0x02 : 0) | ((s_role & 3) << 2) |
           (s_usb_sessions ? 0x10 : 0) | (s_gps_present ? 0x20 : 0) | (s_ota_active ? 0x40 : 0) |
           (linked && s_holder_ios ? 0x80 : 0);
    b[2] = (uint8_t)s_temp_c;
    b[3] = n & 0xFF; b[4] = n >> 8;
    b[5] = (uint8_t)(((fw_maj & 0xF) << 4) | (fw_min & 0xF)); b[6] = (uint8_t)fw_pat;
    b[7] = s_addr_tail[0]; b[8] = s_addr_tail[1];
    size_t len = BEACON_FIXED;
    if (with_holder && linked) {
        size_t h = strlen(s_holder);
        memcpy(b + len, s_holder, h);
        len += h;
    }
    return len;
}

static uint8_t s_mfg[2 + BEACON_FIXED + HOLDER_MAX];   // company id + payload, kept alive for the stack

// ─── Advert extras (1.7.3) ───────────────────────────────────────────────
// What other phones can't see otherwise: board type, battery, co-processor, screen, channels, and how the last link
// ended. Both scan responses are full, so this rides in the advert packet itself (flags 3 + 128-bit UUID 18 + this 10
// = 31 bytes): service data under the 16-bit UUID 0x5744, two 6-byte frames swapped every beacon tick (5 s):
//   [0] b0 frame (0 A, 1 B) · b1-2 how the last link ended (0 no link since boot, 1 released by its host on purpose,
//       2 lost) · b3 a screen is fitted · b4-5 co-processor radio (0 none, 1 off, 2 BT Classic, 3 Wi-Fi) · b6-7 0
//   A: [1] board (board_t) · [2] battery % (0xFF = no gauge / unknown) · [3-5] channel bits 0-23
//   B: [1-3] channel bits 24-41 · [4-5] 0
// Channel bit i = EXT_CH[i] is in this board's channel plan.
#define EXT_UUID16 0x5744
static const uint8_t EXT_CH[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                                  36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                  132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177 };
static uint8_t s_ext_frame = 0;
static uint8_t s_ext[2 + 6];         // UUID + frame, kept alive for the stack

static void ext_build(void) {
    uint8_t plan[MAX_CHANNELS];
    size_t n = plan_copy(plan);
    uint64_t bits = 0;
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < sizeof(EXT_CH); j++)
            if (EXT_CH[j] == plan[i]) { bits |= 1ULL << j; break; }
    uint8_t cp = 0;
#if CONFIG_IDF_TARGET_ESP32C5 && UART_LINK   // the co-processor link (Biscuit Pro / Ultra)
    if (s_cp_present && s_cp_scan) cp = !strcmp(s_cp_mode, "btc") ? 2 : !strcmp(s_cp_mode, "wifi") ? 3 : 1;
#endif
    bool screen = s_board == BOARD_KOKO_V8 || s_board == BOARD_MINI_V3 || s_board == BOARD_AWOK_DUAL_C5 ||
                  s_board == BOARD_POLTERGEIST || s_board == BOARD_WAVESHARE_AMOLED_206;
    uint8_t *b = s_ext + 2;
    memset(b, 0, 6);
    s_ext[0] = EXT_UUID16 & 0xFF; s_ext[1] = EXT_UUID16 >> 8;
    b[0] = (s_ext_frame & 1) | ((s_link_end & 3) << 1) | (screen ? 0x08 : 0) | (cp << 4);
    if (!s_ext_frame) {
        int pct = s_has_batt ? s_batt_pct : -1;
        b[1] = (uint8_t)s_board;
        b[2] = pct < 0 ? 0xFF : (uint8_t)(pct > 100 ? 100 : pct);
        b[3] = bits & 0xFF; b[4] = (bits >> 8) & 0xFF; b[5] = (bits >> 16) & 0xFF;
    } else {
        b[1] = (bits >> 24) & 0xFF; b[2] = (bits >> 32) & 0xFF; b[3] = (bits >> 40) & 0xFF;
    }
}

static void ext_fields(struct ble_hs_adv_fields *adv) {
    ext_build();
    adv->svc_data_uuid16 = s_ext;
    adv->svc_data_uuid16_len = sizeof(s_ext);
}

// Free board: connectable, the service UUID up front, name + status in the scan response (exactly 31 bytes).
static void set_free_fields(void) {
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    ext_fields(&adv);
    if (ble_gap_adv_set_fields(&adv) != 0) ESP_LOGE(TAG, "adv fields rejected");
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    s_mfg[0] = 0xFF; s_mfg[1] = 0xFF;
    size_t n = beacon_payload(s_mfg + 2, false);
    if (2 + rsp.name_len + 4 + n <= BLE_HS_ADV_MAX_SZ) { rsp.mfg_data = s_mfg; rsp.mfg_data_len = 2 + n; }
    (void)ble_gap_adv_rsp_set_fields(&rsp);
}

// Linked board: the status UUID (apps that predate it ignore it) + status and holder in the scan response.
static void set_busy_fields(void) {
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&STATUS_UUID;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    ext_fields(&adv);
    if (ble_gap_adv_set_fields(&adv) != 0) ESP_LOGE(TAG, "beacon fields rejected");
    struct ble_hs_adv_fields rsp = {0};
    s_mfg[0] = 0xFF; s_mfg[1] = 0xFF;
    rsp.mfg_data = s_mfg;
    rsp.mfg_data_len = 2 + beacon_payload(s_mfg + 2, true);
    (void)ble_gap_adv_rsp_set_fields(&rsp);
}

static void start_advertising(void) {
    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();   // the linked-board beacon, if it was still up
    set_free_fields();
    struct ble_gap_adv_params params = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    if (ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap_event, NULL) != 0) {
        ESP_LOGE(TAG, "advertising failed to start");
        return;
    }
    ESP_LOGI(TAG, "advertising as '%s'", s_name);
}

// While a phone or a USB host holds the board: keep telling everyone nearby it's taken, and by whom.
static void start_beacon(void) {
    if (!host_linked()) return;
    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
    set_busy_fields();
    struct ble_gap_adv_params params = { .conn_mode = BLE_GAP_CONN_MODE_NON, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                         .itvl_min = 1600, .itvl_max = 1760 };   // 1-1.1 s: barely any airtime
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, on_gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "status beacon didn't start: %d", rc);
}

// Every 5 s (and right after `iam`): fresh temperature, and new advertising data when anything in it changed.
static void beacon_tick(struct ble_npl_event *ev) {
    (void)ev;
    heap_check("beacon tick");
#if SOC_TEMP_SENSOR_SUPPORTED
    float t;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) s_temp_c = (int8_t)(t < -127 ? -127 : t > 127 ? 127 : t);
#endif
    uint8_t b[BEACON_FIXED + HOLDER_MAX];
    bool linked = host_linked();
    size_t n = beacon_payload(b, linked);
    s_ext_frame ^= 1;   // the other half of the advert extras, so every tick rewrites the advert
    if (n != s_beacon_last_len || memcmp(b, s_beacon_last, n) != 0) {
        memcpy(s_beacon_last, b, n); s_beacon_last_len = n;
        if (linked) start_beacon();
        else if (ble_gap_adv_active()) set_free_fields();
    } else if (linked && !ble_gap_adv_active()) {
        start_beacon();
    } else if (ble_gap_adv_active()) {
        if (linked) set_busy_fields(); else set_free_fields();
    }
    ble_npl_callout_reset(&s_beacon_tick, ble_npl_time_ms_to_ticks32(5000));
}

static int on_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) { start_advertising(); return 0; }
        s_conn_handle = event->connect.conn_handle;
        s_conns++;
        offline_end();
        s_cmd_tail = s_cmd_head;   // nothing from the previous phone runs on this link
        s_cp_sinks &= (uint8_t)~SINK_BLE;   // co-processor results only after this link asks (`coproc`)
        s_holder[0] = '\0'; s_holder_ios = false;
        s_release_req = false;
        s_conn_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_notify_enabled = false;
        if (!s_usb_sessions) {
            s_role = ROLE_WIFI;
            s_switch_pending = false;
            plan_reset_to_all();
        }
        ESP_LOGI(TAG, "phone connected");
        heap_check("ble connect");
        led_storm();
        start_beacon();
        request_link_params(false);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason %d)", event->disconnect.reason);
        heap_check("ble disconnect");
        s_last_disc_reason = (uint32_t)event->disconnect.reason;
        s_last_disc_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_link_end = s_release_req ? LINK_END_RELEASED : LINK_END_LOST;   // told to the next phone in the advert extras
        s_release_req = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        s_cp_sinks &= (uint8_t)~SINK_BLE;
        if (s_ota_active && s_ota_src == SINK_BLE) s_ota_abort_req = true;
        if (s_usb_sessions) {   // USB holds the board: scanning carries on, the beacon keeps naming the holder
            start_beacon();
            return 0;
        }
        s_holder[0] = '\0'; s_holder_ios = false;
        s_switch_pending = false;
        stream_stop();
        s_role = ROLE_WIFI;
        plan_reset_to_all();
        start_advertising();
        offline_begin();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle != s_tx_val_handle) return 0;
        s_notify_enabled = event->subscribe.cur_notify;
        ESP_LOGI(TAG, "notifications %s", s_notify_enabled ? "on" : "off");
        heap_check("subscribe");
        if (s_notify_enabled) host_joined(SINK_BLE);
        else if (!s_usb_sessions) stream_stop();
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", event->mtu.value);
        heap_check("mtu");
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        send_conn_line(event->conn_update.status);
        return 0;

    default:
        return 0;
    }
}

// A phone that's already waiting can connect the instant the board advertises. A link that lands while the radios are
// still starting up corrupts memory inside the Wi-Fi/Bluetooth stack (bench 2026-10-22: ~1 in 6 boots, crash ~15 s
// later), so the first advertising waits until startup has settled.
#if CONFIG_WARDRIVEESP_BENCH_NO_ADV_SETTLE
#define ADV_SETTLE_MS 1   // bench: 1.6.3 timing, to reproduce the early-link corruption
#else
#define ADV_SETTLE_MS 2000
#endif
static struct ble_npl_callout s_adv_settle;

/// Free board: connectable advertising. Held over USB: the busy beacon, and no phone can link (USB-only, 1.6.6).
static void advertise_for_state(void) {
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) return;
    if (s_usb_sessions) start_beacon(); else start_advertising();
}

static void adv_settle_done(struct ble_npl_event *ev) {
    (void)ev;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active()) advertise_for_state();
}

// A USB session opened or closed (posted by usb_hello / usb_session_end, run on the BLE host task). A board held over
// USB lets its Bluetooth phone go (the disconnect then raises the busy beacon); the last session closing makes it
// connectable again so phones relink by themselves.
static void usb_link_switch(struct ble_npl_event *ev) {
    (void)ev;
    if (s_usb_sessions && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "USB host holds the board: letting the Bluetooth phone go");
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    if ((uint32_t)(esp_timer_get_time() / 1000) < ADV_SETTLE_MS) return;   // adv_settle_done advertises
    advertise_for_state();
}

static void on_ble_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0) { ESP_LOGE(TAG, "no BLE address"); return; }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    uint8_t addr[6] = {0};
    if (ble_hs_id_copy_addr(s_own_addr_type, addr, NULL) == 0)
        snprintf(s_name, sizeof(s_name), NAME_PREFIX "-%02X%02X", addr[1], addr[0]);
    s_addr_tail[0] = addr[1]; s_addr_tail[1] = addr[0];
    (void)ble_svc_gap_device_name_set(s_name);
    s_host_task = xTaskGetCurrentTaskHandle();   // on_ble_sync runs on the host task
    if (!s_link_ready) {   // once per Bluetooth start (a host reset can sync twice): they live on the host's event queue
        ble_npl_callout_init(&s_adv_settle, nimble_port_get_dflt_eventq(), adv_settle_done, NULL);
        ble_npl_callout_init(&s_beacon_tick, nimble_port_get_dflt_eventq(), beacon_tick, NULL);
        ble_npl_callout_init(&s_link_switch, nimble_port_get_dflt_eventq(), usb_link_switch, NULL);
        ble_npl_callout_init(&s_cmd_drain, nimble_port_get_dflt_eventq(), cmd_drain, NULL);
        s_cmd_tail = s_cmd_head;
        s_link_ready = true;
    }
    s_ble_synced = true;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000);
    if (up < ADV_SETTLE_MS) ble_npl_callout_reset(&s_adv_settle, ble_npl_time_ms_to_ticks32(ADV_SETTLE_MS - up));
    else advertise_for_state();   // after a USB session or a host reset long after boot: advertise right away
    ble_npl_callout_reset(&s_beacon_tick, 0);   // first temperature reading now, before the first advertising
    ota_mark_valid_if_pending();   // Wi-Fi + BLE came up: keep this firmware (otherwise the next boot rolls back)
    if (s_boot_wd) esp_timer_stop(s_boot_wd);
    boot_stage(STAGE_READY);
}

static void on_ble_reset(int reason) { s_ble_synced = false; ESP_LOGW(TAG, "BLE host reset: %d", reason); }

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/// Controller + host up with the service registered (boot, and when Bluetooth comes back after a USB session).
static esp_err_t ble_stack_init(void) {
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SERVICES));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SERVICES));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(s_name));
    ble_hs_cfg.sync_cb = on_ble_sync;
    ble_hs_cfg.reset_cb = on_ble_reset;
    s_bt_up = true;
    return ESP_OK;
}

// ─── Bluetooth only when it's in use (1.6.6) ──────────────────────────────
// Bluetooth runs while a phone may link (no USB host) or the Bluetooth scan is the chosen mode. A USB host otherwise
// gets the board with the controller off: no Wi-Fi/Bluetooth coexistence, so every channel gets its full dwell. It
// comes back when the last USB session closes. While it's off there is no status beacon.
// Stopping runs here, never on the host task: nimble_port_stop() waits for that task to finish.
static TaskHandle_t s_btpwr_task;

static bool bt_wanted(void) { return !s_usb_sessions || s_role == ROLE_BT || s_bench_keep_bt; }
static void bt_power_kick(void) { if (s_btpwr_task) xTaskNotifyGive(s_btpwr_task); }

static void bt_power_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (bt_wanted() == s_bt_up || s_ota_active || s_rebooting) continue;
        if (s_bt_up) {
            if (s_bt_task) continue;   // the Bluetooth scan is still sending what it has
            for (int t = 0; s_conn_handle != BLE_HS_CONN_HANDLE_NONE && t < 100; t++) vTaskDelay(pdMS_TO_TICKS(20));
            xSemaphoreTake(s_cmd_lock, portMAX_DELAY);   // no callout reset can race the teardown
            bool off = !bt_wanted(), had_callouts = s_link_ready;
            if (off) { s_link_ready = false; s_ble_synced = false; s_bt_up = false; }
            xSemaphoreGive(s_cmd_lock);
            if (!off) continue;
            xSemaphoreTake(s_send_lock, portMAX_DELAY);   // a notify already in flight finishes; later ones see s_bt_up
            xSemaphoreGive(s_send_lock);
            nimble_port_stop();
            if (had_callouts) {   // our timers fire from esp_timer: one left armed would run into the freed event queue
                struct ble_npl_callout *co[] = { &s_beacon_tick, &s_link_switch, &s_adv_settle, &s_cmd_drain };
                for (size_t i = 0; i < sizeof(co) / sizeof(co[0]); i++) { ble_npl_callout_stop(co[i]); ble_npl_callout_deinit(co[i]); }
            }
            nimble_port_deinit();   // also disables and frees the controller
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_notify_enabled = false;
            ESP_LOGI(TAG, "Bluetooth off: a USB host has the board (controller %d, heap %lu)",
                     (int)esp_bt_controller_get_status(), (unsigned long)esp_get_free_heap_size());
        } else {
            if (sdrun_mounted()) {   // an offline run is about to share the heap with Bluetooth: capture's queue goes first
                for (int t = 0; s_cap_task && t < 50; t++) vTaskDelay(pdMS_TO_TICKS(20));   // capture still draining
                xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
                if (!host_linked() && !s_ota_active) free_idle_queues();
                xSemaphoreGive(s_cmd_lock);
            }
            esp_err_t err = ble_stack_init();
            if (err != ESP_OK) { ESP_LOGE(TAG, "Bluetooth didn't come back: %s", esp_err_to_name(err)); continue; }
            nimble_port_freertos_init(ble_host_task);
            ESP_LOGI(TAG, "Bluetooth on (heap %lu)", (unsigned long)esp_get_free_heap_size());
        }
    }
}

static void wifi_bring_up(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Crank the radio to its hardware/regulatory ceiling. Wardriving's 2.4 GHz scan is ACTIVE — the board sends
    // probe requests — so a stronger TX makes distant APs hear the probe and reply, catching more of them per
    // sweep. Only 2.4 GHz probes use it: 5 GHz (DFS 52-144 included) is passive and never transmits. The driver
    // clamps the request to what the chip and country setting allow (units of 0.25 dBm; 84 is the API max). RX
    // sensitivity is fixed silicon on these ESP32 parts — there is no external PA/LNA to enable — so this is the
    // one knob that widens coverage. Set after esp_wifi_start(), which resets it to the default.
    if (esp_wifi_set_max_tx_power(84) == ESP_OK) {
        int8_t p = 0;   // 0.25 dBm units; print as whole + quarter dBm without %f (newlib-nano may drop it)
        if (esp_wifi_get_max_tx_power(&p) == ESP_OK)
            ESP_LOGI(TAG, "max TX power %d units (%d.%02d dBm)", p, p / 4, (p % 4) * 25);
    }
#if SOC_WIFI_SUPPORT_5G
    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    s_has_5g = (err == ESP_OK);
    if (!s_has_5g) ESP_LOGW(TAG, "dual-band mode unavailable (%s), 2.4 GHz only", esp_err_to_name(err));
#endif
    open_all_channels();
    build_supported_channels();
    plan_reset_to_all();
    ESP_LOGI(TAG, "%s: %u channels (%s)", CONFIG_IDF_TARGET, (unsigned)s_supported_n, s_has_5g ? "2.4 + 5 GHz" : "2.4 GHz");
}

// A reset that skips esp_restart() (the USB serial reset that esptool and USB hosts send, a watchdog core reset) leaves
// the Wi-Fi and Bluetooth MACs running from the last boot. When PHY calibration turns the modem clocks back on, the old
// Bluetooth MAC finishes a DMA into buffers the last boot owned, which this boot's heap has handed to someone else: the
// boot-time heap corruption behind the wd_health crash (bench 2026-10-24, heap poisoning: the same block every time,
// written during PHY calibration, before this boot's Bluetooth exists). Pulse every modem reset first, as esp_restart does.
static void modem_reset_stale(void) {
#if SOC_MODEM_CLOCK_SUPPORTED
    modem_clock_module_enable(PERIPH_WIFI_MODULE);
    modem_clock_module_enable(PERIPH_BT_MODULE);
    modem_syscon_ll_reset_all(&MODEM_SYSCON);
    modem_clock_module_disable(PERIPH_BT_MODULE);
    modem_clock_module_disable(PERIPH_WIFI_MODULE);
#endif
}

static long stack_free(TaskHandle_t t) { return t ? (long)uxTaskGetStackHighWaterMark(t) : -1; }

/// WD:HEALTH every 15 s while a host listens: memory, the least stack each task has had left (bytes), the link.
static void health_task(void *arg) {
    (void)arg;
    uint32_t last = 0, last_batt = 0;
    bool boot_run_checked = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        heap_check("health 1 s");
        s_heap_min_seen = esp_get_minimum_free_heap_size();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_has_batt && now - last_batt >= 30000) { last_batt = now; battery_read(); }   // the I2C gauge only
#if COPROC
        coproc_tick(now);
#endif
        // Powered up with a card and no app: nothing ever linked, so no drop starts a run. Start one as soon as the
        // card mounts (5 s after boot); an app that links later closes it like any other.
        if (!boot_run_checked && (host_linked() || sdrun_mounted())) {
            boot_run_checked = true;
            if (!host_linked() && !sdrun_active()) {
                ESP_LOGI(TAG, "no app linked since boot");
                offline_begin();
            }
        }
        if (sdrun_active() && s_cap_queue && !s_cap_task) {   // a PC's capture left its 8 KB queue behind
            xSemaphoreTake(s_cmd_lock, portMAX_DELAY);   // no capture can start while it goes
            if (!host_linked() && !s_ota_active) free_idle_queues();
            xSemaphoreGive(s_cmd_lock);
        }
        // Offline run: Bluetooth coming back up after a USB session can leave too little heap for the scan's stack
        // at the moment of the drop (bench: "scan task create failed"), so keep trying once a second.
        if (sdrun_active() && !host_linked() && !s_ota_active && !s_streaming && !s_scan_task && !s_bt_task &&
            !s_cap_task && !s_csi_task) stream_start();
        bool listening = (s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_notify_enabled) || s_usb_sessions;
        if (!listening || now - last < 15000) continue;
        last = now;
        long scan = stack_free(s_scan_task), bt = stack_free(s_bt_task), cap = stack_free(s_cap_task);
        long gps = stack_free(s_gps_task), usb = stack_free(s_usb_task), csi = stack_free(s_csi_task);
        long self = (long)uxTaskGetStackHighWaterMark(NULL);   // NULL = this task
        const char *low = (scan >= 0 && scan < 768) ? "wd_scan" : (bt >= 0 && bt < 768) ? "wd_bt" :
                          (cap >= 0 && cap < 768) ? "wd_cap" : (gps >= 0 && gps < 768) ? "wd_gps" :
                          (usb >= 0 && usb < 768) ? "wd_usb" : (csi >= 0 && csi < 768) ? "wd_csi" : "none";
        char batt[96] = "";
        if (s_has_batt && s_batt_pct >= 0) {   // none while unknown
            int n = snprintf(batt, sizeof(batt), " batt=%d batt_mv=%d", s_batt_pct, s_batt_mv);
            int r = s_batt_rate;
            if (s_batt_rate_ok && n > 0)   // %/h with two decimals, without %f (newlib-nano may drop it)
                snprintf(batt + n, sizeof(batt) - n, " batt_rate=%s%d.%02d", r < 0 ? "-" : "", abs(r) / 100, abs(r) % 100);
        }
        int temp = -128;   // the chip's own temperature (1.6.8); the beacon keeps it fresh while Bluetooth is up
#if SOC_TEMP_SENSOR_SUPPORTED
        temp = s_temp_c;
        float tf;
        if ((!s_bt_up || temp == -128) && s_tsens && temperature_sensor_get_celsius(s_tsens, &tf) == ESP_OK) temp = (int)tf;
#endif
        if (temp != -128) snprintf(batt + strlen(batt), sizeof(batt) - strlen(batt), " temp=%d", temp);
        if (csi >= 0) snprintf(batt + strlen(batt), sizeof(batt) - strlen(batt), " stack_csi=%ld", csi);   // CSI role only
        send_line("WD:HEALTH up=%lu heap=%lu heap_min=%lu largest=%lu stack_scan=%ld stack_bt=%ld stack_cap=%ld "
                  "stack_gps=%ld stack_usb=%ld stack_health=%ld low_stack=%s mtu=%u last=%s txdrop=%lu%s",
                  (unsigned long)(now / 1000), (unsigned long)esp_get_free_heap_size(),
                  (unsigned long)s_heap_min_seen,
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), scan, bt, cap, gps, usb, self, low,
                  (unsigned)(s_bt_up && s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ble_att_mtu(s_conn_handle) : 0),
                  crumb_name(s_crumb), (unsigned long)s_tx_dropped, batt);
    }
}

void app_main(void) {
    modem_reset_stale();   // first, before Wi-Fi and Bluetooth start (see above)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_send_lock = xSemaphoreCreateMutex();
    s_cmd_lock = xSemaphoreCreateMutex();
    boot_diag_begin();
    board_init();   // before GPS: a board's own bus (the gauge on 4/5) is off limits to the GPS probe
    wifi_bring_up();
    heap_check("boot: after Wi-Fi start");
    boot_stage(STAGE_WIFI);
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) s_tsens = NULL;
#endif

    heap_check("boot: after temperature sensor");
    ESP_ERROR_CHECK(ble_stack_init());
    heap_check("boot: after Bluetooth init");
    boot_stage(STAGE_BLUETOOTH);   // before the host task starts: on_ble_sync can record STAGE_READY right away
    nimble_port_freertos_init(ble_host_task);
    xTaskCreate(bt_power_task, "wd_btpwr", 4096, NULL, 4, &s_btpwr_task);
#if CONFIG_WARDRIVEESP_GPS_ENABLE
    // Optional GPS receiver: runs continuously, emitting WD:GPS when it has a fix. A board with nothing wired to
    // the UART simply never parses a sentence, never advertises gps, and costs only an idle blocked read.
    xTaskCreate(gps_task, "wd_gps", 6144, NULL, 4, &s_gps_task);
#endif
    usb_link_start();
    xTaskCreate(health_task, "wd_health", 6144, NULL, 3, NULL);
    heap_check("boot done");
    ESP_LOGI(TAG, "WardriveESP %s%s ready", FW_VERSION, FW_TAG);
}
