// WardriveESP — offline runs on the microSD card. See sdrun.h.
// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "sdkconfig.h"
#include "sdrun.h"

#if !CONFIG_IDF_TARGET_ESP32C5 && !CONFIG_IDF_TARGET_ESP32C6
// Only boards with a card and a GPS have a slot: Koko V8, AWOK Dual C5, Marauder Mini v3 (C5) and the Ghost Board (C6).
// The Biscuit Ultra's and the Poltergeist's cards are left alone: no GPS, so their rows would have no place.
void sdrun_init(const char *board_id, const char *board_name, const char *fw) { (void)board_id; (void)board_name; (void)fw; }
void sdrun_init_bus(const char *board_id, const char *board_name, const char *fw, int sck, int miso, int mosi, int cs) {
    (void)board_id; (void)board_name; (void)fw; (void)sck; (void)miso; (void)mosi; (void)cs;
}
bool sdrun_remount(void) { return false; }
bool sdrun_mounted(void) { return false; }
void sdrun_space(uint32_t *total_mb, uint32_t *free_mb) { *total_mb = 0; *free_mb = 0; }
bool sdrun_start(void) { return false; }
void sdrun_stop(void) {}
bool sdrun_active(void) { return false; }
void sdrun_ap(const sdrun_ap_t *ap) { (void)ap; }
uint32_t sdrun_rows(void) { return 0; }
uint32_t sdrun_dropped(void) { return 0; }
const char *sdrun_file(void) { return ""; }
int sdrun_list(sdrun_list_fn fn, void *ctx) { (void)fn; (void)ctx; return -1; }
int sdrun_read(const char *name, uint32_t off, uint8_t *buf, size_t len, uint32_t *size) {
    (void)name; (void)off; (void)buf; (void)len; (void)size; return -1;
}
int sdrun_delete(const char *name) { (void)name; return -1; }
void sdrun_pending(uint32_t *n, uint32_t *bytes) { *n = 0; *bytes = 0; }
uint32_t sdrun_remounts(void) { return 0; }
const char *sdrun_test(void) { return "no_slot"; }
#else

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "lcd.h"

static const char *TAG = "WardriveESP";

#define MOUNT    "/sd"
#define RUNS     MOUNT "/runs"
#define Q_LEN    16         // rows waiting for the writer, which takes them as they come
#define BUF_SZ   1024       // rows gathered in RAM between writes (2 s of rows is well under 1 KB)
#define SD_KHZ   10000      // plenty for a few KB/s, and kind to a bus the screen shares
#define MOUNT_TRIES 3
#define ROW_MAX  256        // longest row (a 32-byte SSID that needs quoting every byte, plus the fields)
#define FLUSH_MS 2000       // pulling power loses at most this much
#define LOG_MS   60000      // the writer's progress line in the log while a run is open
#define OPEN_MS  2000       // a run's file opens this long after the drop: the card stays off the bus while
                            // Bluetooth powers back up (bench: a card that timed out right at that moment)

static const char *s_board_id = "", *s_board_name = "", *s_fw = "";
static sdmmc_card_t *s_card;
static volatile bool s_mounted, s_mounting;
static int s_cs = -1;
static int s_own_sck = -1, s_own_miso = -1, s_own_mosi = -1, s_own_cs = -1;   // sdrun_init_bus: the slot's own bus
static bool s_own_bus_up;
static volatile uint32_t s_total_mb, s_free_mb;
static TaskHandle_t s_task;
static QueueHandle_t s_q;
static char *s_buf;          // BUF_SZ, allocated once a card is mounted
static size_t s_n;
static volatile int s_fd = -1;
static volatile bool s_want;      // a run should be open (set by sdrun_start, cleared by sdrun_stop)
static volatile bool s_active;    // rows are accepted
static volatile uint32_t s_rows, s_dropped;
static char s_file[16];           // "run_<n>.csv" of the current or last run
static uint32_t s_last_flush, s_last_log;
static volatile uint32_t s_start_ms;   // when the current run was asked for
static volatile uint32_t s_remounts;
static volatile uint32_t s_wait_n, s_wait_bytes;   // closed runs on the card (sdrun_pending)   // times the card stopped answering and was mounted again
static volatile bool s_mount_req;      // `sd mount` while the writer runs but the card is lost

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void refresh_space(void) {
    uint64_t total = 0, free = 0;
    lcd_bus_lock();
    esp_err_t e = esp_vfs_fat_info(MOUNT, &total, &free);
    lcd_bus_unlock();
    if (e == ESP_OK) { s_total_mb = (uint32_t)(total >> 20); s_free_mb = (uint32_t)(free >> 20); }
}

/// Count the closed runs in /runs: after a mount, a close and a delete.
static void recount(void) {
    uint32_t n = 0, bytes = 0;
    lcd_bus_lock();
    DIR *d = opendir(RUNS);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncasecmp(e->d_name, "run_", 4) || (s_fd >= 0 && !strcasecmp(e->d_name, s_file))) continue;
            char path[48];
            struct stat st;
            snprintf(path, sizeof(path), RUNS "/%.23s", e->d_name);
            if (stat(path, &st) == 0) { n++; bytes += (uint32_t)st.st_size; }
        }
        closedir(d);
    }
    lcd_bus_unlock();
    s_wait_n = n;
    s_wait_bytes = bytes;
}

/// The slot's SPI host and chip select: the screen's bus (Koko V8, AWOK, Mini v3), or a bus of its own (Ghost Board),
/// set up here the first time.
static bool slot(int *host, int *cs) {
    if (lcd_sd_slot(host, cs)) return true;
    if (s_own_cs < 0) return false;
    if (!s_own_bus_up) {
        gpio_set_direction(s_own_cs, GPIO_MODE_OUTPUT);   // the card stays deselected while the bus comes up
        gpio_set_level(s_own_cs, 1);
        spi_bus_config_t bc = { .mosi_io_num = s_own_mosi, .miso_io_num = s_own_miso, .sclk_io_num = s_own_sck,
                                .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096 };
        esp_err_t e = spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) { ESP_LOGW(TAG, "sd: bus init failed (%s)", esp_err_to_name(e)); return false; }
        s_own_bus_up = true;
    }
    if (host) *host = SPI2_HOST;
    if (cs) *cs = s_own_cs;
    return true;
}

static bool mount(void) {
    int host, cs;
    if (!slot(&host, &cs)) return false;
    s_cs = cs;
    sdmmc_host_t h = SDSPI_HOST_DEFAULT();
    h.slot = host;
    h.max_freq_khz = SD_KHZ;
    sdspi_device_config_t d = SDSPI_DEVICE_CONFIG_DEFAULT();
    d.host_id = (spi_host_device_t)host;
    d.gpio_cs = (gpio_num_t)cs;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 2,   // the run + one reader
                                            .allocation_unit_size = 16 * 1024 };
    esp_err_t e = ESP_FAIL;
    // A card left mid-transfer by a reset (its power never dropped) can garble its first answers: try a few times.
    for (int i = 0; i < MOUNT_TRIES && e != ESP_OK; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(300));
        lcd_bus_lock();
        e = esp_vfs_fat_sdspi_mount(MOUNT, &h, &d, &mc, &s_card);
        if (e != ESP_OK) {   // no card (or not FAT): its select back to idle so it stays off the screen's bus
            gpio_set_direction(cs, GPIO_MODE_OUTPUT);
            gpio_set_level(cs, 1);
        }
        lcd_bus_unlock();
        if (e == ESP_ERR_TIMEOUT) break;   // nothing answers: no card, no point retrying
    }
    if (e != ESP_OK) {
        ESP_LOGI(TAG, "sd: no card mounted (%s)", esp_err_to_name(e));
        return false;
    }
    if (!s_q) s_q = xQueueCreate(Q_LEN, sizeof(sdrun_ap_t));
    if (!s_buf) s_buf = heap_caps_malloc(BUF_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_q || !s_buf) {
        ESP_LOGW(TAG, "sd: no memory for the writer");
        lcd_bus_lock();
        esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
        lcd_bus_unlock();
        s_card = NULL;
        return false;
    }
    lcd_bus_lock();
    mkdir(RUNS, 0777);
    lcd_bus_unlock();
    refresh_space();
    s_mounted = true;
    recount();
    ESP_LOGI(TAG, "sd: card mounted, %s, %lu MB, %lu MB free", s_card->cid.name, (unsigned long)s_total_mb,
             (unsigned long)s_free_mb);
    return true;
}

/// The card stopped answering (a reset or a brown-out puts it back in its idle state): drop the mount and do it
/// again. Any open file is gone; the caller opens it again.
static bool remount(void) {
    s_remounts++;
    ESP_LOGW(TAG, "sd: card stopped answering, mounting it again (%lu)", (unsigned long)s_remounts);
    lcd_bus_lock();
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    lcd_bus_unlock();
    s_card = NULL;
    s_mounted = false;
    return mount();
}

// ── rows ──
static void fixed(char *b, size_t cap, double v, long scale, int dec) {   // no %f: the build may use nano printf
    bool neg = v < 0;
    if (neg) v = -v;
    long s = (long)(v * (double)scale + 0.5);
    snprintf(b, cap, "%s%ld.%0*ld", neg ? "-" : "", s / scale, dec, s % scale);
}

static const char *wigle_auth(const char *t) {
    if (!strcmp(t, "OPEN"))      return "[ESS]";
    if (!strcmp(t, "WEP"))       return "[WEP][ESS]";
    if (!strcmp(t, "WPA"))       return "[WPA-PSK-TKIP][ESS]";
    if (!strcmp(t, "WPA2"))      return "[WPA2-PSK-CCMP][ESS]";
    if (!strcmp(t, "WPA3"))      return "[WPA3-SAE-CCMP][ESS]";
    if (!strcmp(t, "WPA2_WPA3")) return "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]";
    return "[ESS]";
}

// FirstSeen in UTC. Until the clock is set (GPS time, or the app's `time`), the board's uptime from 1970-01-01, which
// an importer can shift once it knows when the run started.
static void first_seen(char *b, size_t cap) {
    time_t t = time(NULL);
    if (t < 1700000000) t = (time_t)(esp_timer_get_time() / 1000000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(b, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static size_t csv_field(char *out, const char *s) {   // RFC 4180: quoted when it holds a comma, quote or line break
    bool q = strpbrk(s, ",\"\r\n") != NULL;
    size_t n = 0;
    if (q) out[n++] = '"';
    for (; *s; s++) {
        if (*s == '"') out[n++] = '"';
        out[n++] = *s;
    }
    if (q) out[n++] = '"';
    out[n] = '\0';
    return n;
}

static bool write_out(void) {
    lcd_bus_lock();
    ssize_t w = write(s_fd, s_buf, s_n);
    int f = fsync(s_fd);
    lcd_bus_unlock();
    return w == (ssize_t)s_n && f == 0;
}

static bool flush(void) {
    if (s_fd < 0 || !s_n) return true;
    bool ok = write_out();
    if (!ok && remount()) {   // once: mount the card again and append to the same file
        char path[48];
        snprintf(path, sizeof(path), RUNS "/%s", s_file);
        lcd_bus_lock();
        s_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
        lcd_bus_unlock();
        ok = s_fd >= 0 && write_out();
    }
    s_last_flush = now_ms();
    if (!ok) ESP_LOGW(TAG, "sd: write failed (%u bytes), closing %s", (unsigned)s_n, s_file);
    s_n = 0;
    return ok;
}

static void close_run(void) {
    if (s_fd < 0) return;
    flush();
    lcd_bus_lock();
    if (s_fd >= 0) close(s_fd);
    lcd_bus_unlock();
    s_fd = -1;
    refresh_space();
    recount();
    ESP_LOGI(TAG, "sd: %s closed, %lu rows, %lu dropped, %lu MB free", s_file, (unsigned long)s_rows,
             (unsigned long)s_dropped, (unsigned long)s_free_mb);
}

static void append(const sdrun_ap_t *r) {
    if (s_n + ROW_MAX > BUF_SZ && !flush()) { close_run(); s_active = s_want = false; return; }
    char ssid[2 * sizeof(r->ssid) + 3], when[24], lat[16] = "0", lon[16] = "0", alt[16] = "0", acc[12] = "0";
    csv_field(ssid, r->ssid);
    first_seen(when, sizeof(when));
    if (r->fix) {
        fixed(lat, sizeof(lat), r->lat, 1000000, 6);
        fixed(lon, sizeof(lon), r->lon, 1000000, 6);
        fixed(alt, sizeof(alt), r->alt, 10, 1);
        fixed(acc, sizeof(acc), r->acc_m, 10, 1);
    }
    const uint8_t *b = r->bssid;
    int n = snprintf(s_buf + s_n, BUF_SZ - s_n, "%02x:%02x:%02x:%02x:%02x:%02x,%s,%s,%s,%u,%d,%s,%s,%s,%s,WIFI\n",
                     b[0], b[1], b[2], b[3], b[4], b[5], ssid, wigle_auth(r->auth), when, (unsigned)r->ch, r->rssi,
                     lat, lon, alt, acc);
    if (n > 0 && s_n + (size_t)n < BUF_SZ) { s_n += (size_t)n; s_rows++; }
    else s_dropped++;
}

// Run numbers only go up, kept in NVS beside the card's own files: an app that took run_3 and had it deleted never
// sees a different run_3 from this board.
static int next_number(void) {
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("iosesp", NVS_READONLY, &h) == ESP_OK) { nvs_get_u32(h, "sd_next", &v); nvs_close(h); }
    return (int)v;
}

static void save_next_number(int next) {
    nvs_handle_t h;
    if (nvs_open("iosesp", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "sd_next", (uint32_t)next);
    nvs_commit(h);
    nvs_close(h);
}

static int create_run(void) {
    int next = next_number();
    if (next < 1) next = 1;
    lcd_bus_lock();
    DIR *d = opendir(RUNS);
    if (!d) { mkdir(RUNS, 0777); d = opendir(RUNS); }
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncasecmp(e->d_name, "run_", 4)) continue;
            int k = atoi(e->d_name + 4);
            if (k >= next) next = k + 1;
        }
        closedir(d);
    }
    snprintf(s_file, sizeof(s_file), "run_%d.csv", next);
    save_next_number(next + 1);
    char path[48];
    snprintf(path, sizeof(path), RUNS "/%s", s_file);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    lcd_bus_unlock();
    return fd;
}

static void open_run(void) {
    s_fd = create_run();
    if (s_fd < 0 && remount()) s_fd = create_run();
    if (s_fd < 0) {
        ESP_LOGW(TAG, "sd: can't create %s", s_file);
        s_active = s_want = false;
        return;
    }
    s_n = (size_t)snprintf(s_buf, BUF_SZ,
                           "WigleWifi-1.4,appRelease=%s,model=%s,release=%s,device=WardriveESP,display=%s,board=%s,"
                           "brand=WardriveESP\n"
                           "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
                           "AccuracyMeters,Type\n", s_fw, s_board_name, s_fw,
                           lcd_size()[0] ? lcd_size() : "none", s_board_id);
    flush();   // the header is on the card before the first row
    s_last_log = now_ms();
    ESP_LOGI(TAG, "sd: offline run %s started", s_file);
}

static void writer_task(void *arg) {
    (void)arg;
    if (!s_mounted && esp_timer_get_time() < 8000000) vTaskDelay(pdMS_TO_TICKS(5000));   // boot: radios first
    if (!mount()) {
        s_mounting = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_mounting = false;
    for (;;) {
        if (!s_mounted && s_mount_req) { s_mount_req = false; (void)mount(); }   // `sd mount` after a lost card
        if (s_want && s_fd < 0) {   // a new run, before its file opens: its rows wait in the queue
            if (s_mounted && now_ms() - s_start_ms >= OPEN_MS) open_run();
            else { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        }
        sdrun_ap_t r;
        bool got = xQueueReceive(s_q, &r, pdMS_TO_TICKS(250)) == pdTRUE;
        if (got && s_fd >= 0) append(&r);
        else if (got && s_want) s_dropped++;
        if (!s_want && s_fd >= 0 && !uxQueueMessagesWaiting(s_q)) close_run();
        uint32_t now = now_ms();
        if (s_fd >= 0 && s_n && now - s_last_flush >= FLUSH_MS && !flush()) { close_run(); s_active = s_want = false; }
        if (s_fd >= 0 && now - s_last_log >= LOG_MS) {
            s_last_log = now;
            ESP_LOGI(TAG, "sd: %s rows=%lu dropped=%lu heap=%lu heap_min=%lu largest=%lu", s_file, (unsigned long)s_rows,
                     (unsigned long)s_dropped, (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        }
    }
}

static bool spawn(void) {
    if (s_task || s_mounted || (!lcd_sd_slot(NULL, NULL) && s_own_cs < 0)) return false;
    s_mounting = true;
    if (xTaskCreate(writer_task, "wd_sd", 4096, NULL, 2, &s_task) != pdPASS) { s_task = NULL; s_mounting = false; return false; }
    return true;
}

// ── API ──
void sdrun_init(const char *board_id, const char *board_name, const char *fw) {
    s_board_id = board_id ? board_id : "";
    s_board_name = board_name ? board_name : "";
    s_fw = fw ? fw : "";
    (void)spawn();
}

void sdrun_init_bus(const char *board_id, const char *board_name, const char *fw, int sck, int miso, int mosi, int cs) {
    if (s_own_cs >= 0 || s_task || s_mounted) return;
    s_own_sck = sck; s_own_miso = miso; s_own_mosi = mosi; s_own_cs = cs;
    sdrun_init(board_id, board_name, fw);
}

bool sdrun_remount(void) {
    if (s_task && !s_mounted && !s_mounting) { s_mount_req = true; return true; }
    return spawn();
}
bool sdrun_mounted(void) { return s_mounted; }
void sdrun_space(uint32_t *total_mb, uint32_t *free_mb) { *total_mb = s_total_mb; *free_mb = s_free_mb; }

bool sdrun_start(void) {
    if (!s_mounted) return false;
    if (!s_want && s_fd < 0) { s_rows = 0; s_dropped = 0; s_start_ms = now_ms(); }   // a run still closing carries on
    s_want = true;
    s_active = true;
    return true;
}

void sdrun_stop(void) {
    s_active = false;
    s_want = false;
}

bool sdrun_active(void) { return s_active; }

void sdrun_ap(const sdrun_ap_t *ap) {
    if (!s_active || !s_q) return;
    if (xQueueSend(s_q, ap, 0) != pdTRUE) s_dropped++;
}

uint32_t sdrun_rows(void) { return s_rows; }
uint32_t sdrun_dropped(void) { return s_dropped; }
const char *sdrun_file(void) { return s_file; }

static bool name_ok(const char *n) { return n && *n && strlen(n) < 24 && !strchr(n, '/') && !strstr(n, ".."); }

int sdrun_list(sdrun_list_fn fn, void *ctx) {
    if (!s_mounted) return -1;
    int count = 0;
    lcd_bus_lock();
    DIR *d = opendir(RUNS);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            char path[48];
            struct stat st;
            snprintf(path, sizeof(path), RUNS "/%.23s", e->d_name);
            uint32_t size = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
            lcd_bus_unlock();   // the reply goes out without holding the bus
            fn(e->d_name, size, ctx);
            lcd_bus_lock();
            count++;
        }
        closedir(d);
    }
    lcd_bus_unlock();
    return count;
}

int sdrun_read(const char *name, uint32_t off, uint8_t *buf, size_t len, uint32_t *size) {
    if (!s_mounted || !name_ok(name)) return -1;
    char path[48];
    snprintf(path, sizeof(path), RUNS "/%s", name);
    lcd_bus_lock();
    int fd = open(path, O_RDONLY);
    int n = -1;
    if (fd >= 0) {
        struct stat st;
        *size = fstat(fd, &st) == 0 ? (uint32_t)st.st_size : 0;
        n = lseek(fd, (off_t)off, SEEK_SET) == (off_t)off ? (int)read(fd, buf, len) : 0;
        close(fd);
    }
    lcd_bus_unlock();
    return n;
}

int sdrun_delete(const char *name) {
    if (!s_mounted || !name_ok(name)) return -1;
    if (s_fd >= 0 && !strcasecmp(name, s_file)) return -2;
    char path[48];
    snprintf(path, sizeof(path), RUNS "/%s", name);
    lcd_bus_lock();
    int r = unlink(path);
    lcd_bus_unlock();
    if (r == 0) { refresh_space(); recount(); }
    return r == 0 ? 0 : -1;
}

void sdrun_pending(uint32_t *n, uint32_t *bytes) { *n = s_wait_n; *bytes = s_wait_bytes; }

uint32_t sdrun_remounts(void) { return s_remounts; }

const char *sdrun_test(void) {
    if (!s_mounted) return "not_mounted";
    static const char msg[] = "WardriveESP sd test\n";
    char back[sizeof(msg)] = "";
    const char *r = "ok";
    lcd_bus_lock();
    int fd = open(MOUNT "/wdtest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) r = "open_failed";
    else {
        if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1) || fsync(fd) != 0) r = "write_failed";
        close(fd);
    }
    if (!strcmp(r, "ok")) {
        fd = open(MOUNT "/wdtest.txt", O_RDONLY);
        if (fd < 0 || read(fd, back, sizeof(back) - 1) != (ssize_t)(sizeof(msg) - 1) || strcmp(back, msg)) r = "read_failed";
        if (fd >= 0) close(fd);
        unlink(MOUNT "/wdtest.txt");
    }
    lcd_bus_unlock();
    return r;
}
#endif
