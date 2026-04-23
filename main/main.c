/**
 * JC2432W328 — Raw RGB565 Video Player with ESP Rainmaker
 *
 * Video files on SD card (FAT32, root directory):
 *   gif_a.raw  — played when Rainmaker toggle is OFF
 *   gif_b.raw  — played when Rainmaker toggle is ON
 *
 * File format (.raw) — written by companion make_raw.py:
 *   Bytes 0-1: uint16_t width          (little-endian)
 *   Bytes 2-3: uint16_t height         (little-endian)
 *   Bytes 4-5: uint16_t frame_delay_ms (little-endian)
 *   Bytes 6-7: uint16_t frame_count    (little-endian)
 *   Then:      frame_count * (width * height * 2) bytes of big-endian RGB565
 *
 * SD card wiring (SPI3/VSPI — separate from LCD):
 *   MOSI=23  MISO=19  SCK=18  CS=5
 *
 * LCD wiring (SPI2/HSPI):
 *   MOSI=13  SCLK=14  CS=15  DC=2  BL=27
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>   /* read(), fileno() — POSIX I/O used by esp_video */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "driver/ledc.h"
#include "esp_mac.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "esp_rmaker_core.h"
#include "esp_rmaker_standard_types.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_devices.h"
#include "esp_rmaker_ota.h"
#include "esp_rmaker_schedule.h"
#include "esp_rmaker_scenes.h"
#include "esp_rmaker_factory.h"
#include "esp_rmaker_common_events.h"

static const char *TAG = "gif_viewer";

/* ════════════════════════════════════════════════════════
   LCD PINS  (SPI2 / HSPI)
   ════════════════════════════════════════════════════════ */
#define LCD_SCLK     14
#define LCD_MOSI     13
#define LCD_MISO     (-1)
#define LCD_CS       15
#define LCD_DC        2
#define LCD_RST      (-1)
#define LCD_BL       27
#define LCD_W       240
#define LCD_H       320
#define LCD_HOST    SPI2_HOST
#define LCD_CLK_HZ  (80 * 1000 * 1000)

/* ════════════════════════════════════════════════════════
   SD CARD PINS  (SPI3 / VSPI)
   ════════════════════════════════════════════════════════ */
#define SD_HOST     SPI3_HOST
#define SD_MOSI     23
#define SD_MISO     19
#define SD_SCK      18
#define SD_CS        5
/* FIX 1 — Push SD clock from 20 MHz to 40 MHz.
 * This roughly doubles raw read throughput (~2 MB/s → ~4 MB/s),
 * which at 150 KB/frame drops the read time from ~73 ms to ~37 ms.
 * The JC2432W328 SD slot traces are short enough to handle 40 MHz. */
#define SD_CLK_KHZ  40000
#define SD_MOUNT    "/sdcard"

/* ════════════════════════════════════════════════════════
   VIDEO PLAYBACK
   ════════════════════════════════════════════════════════ */
/* FIX 2 — Allocate the largest DMA-capable contiguous block we can get,
 * then divide it into a whole number of full LCD rows.
 * After Rainmaker + BLE init we typically have ~80-100 KB free.
 * We target 64 KB (enough for 170 rows at once vs the old 16),
 * which reduces the number of SPI transactions per frame from 20 to 2.
 * The actual size is determined at runtime from the heap. */
#define PUSH_BUF_TARGET_KB  64
#define PUSH_BUF_BYTES      (PUSH_BUF_TARGET_KB * 1024)

/* Bytes per full LCD row in the push buffer / file */
#define ROW_BYTES           (LCD_W * 2)

/* .raw header is exactly 8 bytes */
#define RAW_HDR_BYTES       8

/* ════════════════════════════════════════════════════════
   GLOBALS
   ════════════════════════════════════════════════════════ */
static esp_lcd_panel_handle_t s_panel  = NULL;
static volatile int           s_active = 0;
static EventGroupHandle_t     s_prov_eg;
#define WIFI_CONNECTED_BIT  BIT0
#define PROV_DONE_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT2

/* ════════════════════════════════════════════════════════
   BACKLIGHT
   ════════════════════════════════════════════════════════ */
static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num   = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 1023,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

/* ════════════════════════════════════════════════════════
   LCD INIT  (SPI2)
   ════════════════════════════════════════════════════════ */
static void lcd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = LCD_MISO,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PUSH_BUF_BYTES + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_DC,
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = LCD_CLK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pcfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    backlight_init();
    ESP_LOGI(TAG, "LCD ready %dx%d", LCD_W, LCD_H);
}

static void lcd_fill(uint16_t colour_rgb565)
{
    static uint16_t line[LCD_W];
    uint16_t be = __builtin_bswap16(colour_rgb565);
    for (int i = 0; i < LCD_W; i++) line[i] = be;
    for (int y = 0; y < LCD_H; y++)
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + 1, line);
}

/* ════════════════════════════════════════════════════════
   SD CARD INIT  (SPI3)
   ════════════════════════════════════════════════════════ */
static sdmmc_card_t *s_card = NULL;

static void sdcard_init(void)
{
    ESP_LOGI(TAG, "SD init: MOSI=%d MISO=%d SCK=%d CS=%d @ %d kHz",
             SD_MOSI, SD_MISO, SD_SCK, SD_CS, SD_CLK_KHZ);

    spi_bus_config_t bus = {
        .mosi_io_num   = SD_MOSI,
        .miso_io_num   = SD_MISO,
        .sclk_io_num   = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = SD_CLK_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = SD_CS;
    slot.host_id = SD_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mnt, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        lcd_fill(0xF800);
        return;
    }
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD mounted at " SD_MOUNT);
}

/* ════════════════════════════════════════════════════════
   PROVISIONING
   ════════════════════════════════════════════════════════ */
static void print_qr_code(const char *service_name, const char *pop)
{
    char payload[200];
    snprintf(payload, sizeof(payload),
        "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
        service_name, pop);
    ESP_LOGI(TAG, "===== PROVISIONING QR =====");
    ESP_LOGI(TAG, "https://rainmaker.espressif.com/qrcode.html?data=%s", payload);
    ESP_LOGI(TAG, "Service: %s  PoP: %s", service_name, pop);
    ESP_LOGI(TAG, "===========================");
}

static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            lcd_fill(0x07E0); break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "Creds: SSID=%s", cfg->ssid);
            lcd_fill(0xFFE0); break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *r =
                (network_prov_wifi_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "WiFi fail: %s",
                (*r == NETWORK_PROV_WIFI_STA_AUTH_ERROR) ? "auth" : "not found");
            lcd_fill(0xF800); break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "WiFi creds accepted"); break;
        case NETWORK_PROV_END:
            network_prov_mgr_deinit(); break;
        default: break;
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_prov_eg, WIFI_CONNECTED_BIT);
    } else if (base == RMAKER_EVENT && id == RMAKER_EVENT_CLAIM_SUCCESSFUL) {
        ESP_LOGI(TAG, "Claimed!");
        lcd_fill(0x001F);
        xEventGroupSetBits(s_prov_eg, PROV_DONE_BIT);
    } else if (base == RMAKER_COMMON_EVENT && id == RMAKER_MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT up (heap: %lu)", (unsigned long)esp_get_free_heap_size());
        xEventGroupSetBits(s_prov_eg, MQTT_CONNECTED_BIT);
    }
}

static bool rmaker_is_claimed(void)
{
    return (esp_rmaker_factory_get_size("client_cert") > 0);
}

static void wifi_prov_init(void)
{
    s_prov_eg = xEventGroupCreate();

    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                               prov_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               prov_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               prov_event_handler, NULL);
    esp_event_handler_register(RMAKER_EVENT, RMAKER_EVENT_CLAIM_SUCCESSFUL,
                               prov_event_handler, NULL);
    esp_event_handler_register(RMAKER_COMMON_EVENT, RMAKER_MQTT_EVENT_CONNECTED,
                               prov_event_handler, NULL);

    network_prov_mgr_config_t prov_cfg = {
        .scheme               = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(prov_cfg));

    bool wifi_provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&wifi_provisioned));

    if (!wifi_provisioned || !rmaker_is_claimed()) {
        if (wifi_provisioned && !rmaker_is_claimed())
            ESP_LOGW(TAG, "WiFi saved but not claimed — re-provisioning");

        lcd_fill(0x07E0);
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char svc[20];
        snprintf(svc, sizeof(svc), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);
        const char *pop = "abcd1234";
        print_qr_code(svc, pop);

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, pop, svc, NULL));

        xEventGroupWaitBits(s_prov_eg, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        lcd_fill(0xFFE0);
        xEventGroupWaitBits(s_prov_eg, PROV_DONE_BIT,
                            pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    } else {
        ESP_LOGI(TAG, "Already provisioned");
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_connect();
        xEventGroupWaitBits(s_prov_eg, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
    }
    ESP_LOGI(TAG, "WiFi ready");
}

/* ════════════════════════════════════════════════════════
   ANIMATION TASK  (core 1)

   Three performance fixes taken directly from esp_video:

   FIX 2 — Single read() per chunk instead of fread() in a loop.
     esp_video calls read(fileno(fp), buf, frame_bytes) for the whole
     frame in one syscall. Without PSRAM we can't hold 150 KB in one
     DMA buffer, so we use the largest chunk that fits. We still use
     read() rather than fread() to avoid per-call VFS overhead.

   FIX 3 — Deadline-based frame timing.
     esp_video uses ets_delay_us(5000) as a fixed small leeway rather
     than a per-frame sleep. Here we timestamp before the read+push,
     measure how long it took, then sleep only the remaining budget.
     If the frame took longer than the delay budget (SD too slow) we
     skip the sleep entirely and play as fast as possible — which is
     what esp_video effectively does.

   FIX 4 — SD clock at 40 MHz (defined in SD_CLK_KHZ above).
   ════════════════════════════════════════════════════════ */
static void anim_task(void *_arg)
{
    int   cur_gif = -1;
    FILE *f       = NULL;
    int   fd      = -1;          /* POSIX fd for read() */

    uint16_t vid_w = 0, vid_h = 0, delay_ms = 33, frame_count = 0;
    size_t   src_row_bytes = 0;
    size_t   frame_bytes   = 0;

    /* Allocate the largest DMA-capable buffer we can get, rounded down
     * to a whole number of LCD rows.  On ESP32 without PSRAM this is
     * typically 64–80 KB after Rainmaker/BT init. */
    size_t buf_bytes = PUSH_BUF_BYTES;
    uint8_t *push_buf = NULL;
    while (buf_bytes >= ROW_BYTES) {
        push_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        if (push_buf) break;
        buf_bytes -= ROW_BYTES;   /* try one row smaller */
    }
    if (!push_buf) {
        ESP_LOGE(TAG, "Cannot allocate any push buffer");
        vTaskDelete(NULL);
        return;
    }
    /* Round down to whole rows */
    int rows_per_chunk = (int)(buf_bytes / ROW_BYTES);
    buf_bytes = (size_t)rows_per_chunk * ROW_BYTES;
    ESP_LOGI(TAG, "Push buffer: %u KB (%d rows/chunk)",
             (unsigned)(buf_bytes / 1024), rows_per_chunk);

    /* Wait for MQTT so TLS gets full heap */
    ESP_LOGI(TAG, "Waiting for MQTT (heap: %lu)",
             (unsigned long)esp_get_free_heap_size());
    xEventGroupWaitBits(s_prov_eg, MQTT_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "MQTT OK — video starting (heap: %lu)",
             (unsigned long)esp_get_free_heap_size());

    while (true) {
        int active = s_active;

        /* ── Open or switch video file ── */
        if (active != cur_gif || !f) {
            cur_gif = active;
            if (f) { fclose(f); f = NULL; fd = -1; }

            lcd_fill(0x0000);

            const char *path = active
                ? SD_MOUNT "/gif_b.raw"
                : SD_MOUNT "/gif_a.raw";

            ESP_LOGI(TAG, "Opening %s (heap: %lu)",
                     path, (unsigned long)esp_get_free_heap_size());

            f = fopen(path, "rb");
            if (!f) {
                ESP_LOGE(TAG, "Cannot open %s", path);
                lcd_fill(0xF800);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            fd = fileno(f);   /* POSIX fd for read() */

            uint8_t hdr[RAW_HDR_BYTES];
            if (read(fd, hdr, RAW_HDR_BYTES) != RAW_HDR_BYTES) {
                ESP_LOGE(TAG, "%s: header read failed", path);
                fclose(f); f = NULL; fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            vid_w       = (uint16_t)(hdr[0] | (hdr[1] << 8));
            vid_h       = (uint16_t)(hdr[2] | (hdr[3] << 8));
            delay_ms    = (uint16_t)(hdr[4] | (hdr[5] << 8));
            frame_count = (uint16_t)(hdr[6] | (hdr[7] << 8));

            if (!vid_w || !vid_h || !frame_count) {
                ESP_LOGE(TAG, "%s: bad header w=%u h=%u fc=%u",
                         path, vid_w, vid_h, frame_count);
                fclose(f); f = NULL; fd = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            src_row_bytes = (size_t)vid_w * 2;
            frame_bytes   = (size_t)vid_h * src_row_bytes;

            ESP_LOGI(TAG, "%s: %ux%u  %u frames  %u ms/frame  %u KB/frame",
                     path, vid_w, vid_h, frame_count, delay_ms,
                     (unsigned)(frame_bytes / 1024));
        }

        /* ── FIX 3: Timestamp the start of this frame ── */
        int64_t frame_start_us = esp_timer_get_time();

        /* ── Push one frame from SD to LCD ── */
        int clip_w = (vid_w < (uint16_t)LCD_W) ? vid_w : LCD_W;
        int clip_h = (vid_h < (uint16_t)LCD_H) ? vid_h : LCD_H;
        bool io_error = false;

        for (int row = 0; row < clip_h && !io_error; row += rows_per_chunk) {
            int chunk = rows_per_chunk;
            if (row + chunk > clip_h) chunk = clip_h - row;
            size_t chunk_bytes = (size_t)chunk * src_row_bytes;

            /* FIX 2: Use POSIX read() — single syscall, no per-call VFS
             * overhead, matches the esp_video approach exactly. */
            ssize_t got = read(fd, push_buf, chunk_bytes);
            if (got != (ssize_t)chunk_bytes) {
                io_error = true;
                break;
            }

            /* Zero-fill columns to the right if video narrower than LCD */
            if (src_row_bytes < (size_t)ROW_BYTES) {
                for (int r = chunk - 1; r >= 0; r--) {
                    uint8_t *row_ptr = push_buf + (size_t)r * ROW_BYTES;
                    uint8_t *src_ptr = push_buf + (size_t)r * src_row_bytes;
                    if (row_ptr != src_ptr)
                        memmove(row_ptr, src_ptr, src_row_bytes);
                    memset(row_ptr + src_row_bytes, 0,
                           ROW_BYTES - src_row_bytes);
                }
            }

            esp_lcd_panel_draw_bitmap(s_panel, 0, row, clip_w, row + chunk,
                                      push_buf);
        }

        /* Skip rows below LCD_H if video taller than display */
        if (!io_error && vid_h > (uint16_t)LCD_H) {
            size_t skip = (size_t)(vid_h - LCD_H) * src_row_bytes;
            lseek(fd, (off_t)skip, SEEK_CUR);
        }

        /* Black-fill rows below video if video shorter than display */
        if (!io_error && vid_h < (uint16_t)LCD_H) {
            memset(push_buf, 0, buf_bytes);
            for (int y = vid_h; y < LCD_H; y += rows_per_chunk) {
                int chunk = rows_per_chunk;
                if (y + chunk > LCD_H) chunk = LCD_H - y;
                esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + chunk,
                                          push_buf);
            }
        }

        /* EOF / short read → loop back to first frame */
        if (io_error) {
            ESP_LOGD(TAG, "Looping video");
            lseek(fd, RAW_HDR_BYTES, SEEK_SET);
        }

        /* FIX 3: Deadline-based timing (esp_video principle).
         * Measure how long read+push actually took, then sleep only
         * the remaining budget.  If we're already over budget (SD is
         * the bottleneck), skip the sleep and play as fast as possible.
         * This prevents the double-counting bug where the old code did:
         *   actual_period = render_time + delay_ms   (always too slow). */
        int64_t elapsed_us = esp_timer_get_time() - frame_start_us;
        int64_t budget_us  = (int64_t)delay_ms * 1000;
        int64_t sleep_us   = budget_us - elapsed_us;
        if (sleep_us > 1000) {
            /* More than 1 ms remaining — use FreeRTOS sleep */
            vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
        }
        /* else: over budget — start next frame immediately */
    }

    free(push_buf);
    if (f) fclose(f);
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════
   RAINMAKER
   ════════════════════════════════════════════════════════ */
static esp_err_t rm_write_cb(const esp_rmaker_device_t *dev,
                              const esp_rmaker_param_t  *param,
                              const esp_rmaker_param_val_t val,
                              void *priv, esp_rmaker_write_ctx_t *ctx)
{
    if (strcmp(esp_rmaker_param_get_name(param), "Answer") == 0) {
        s_active = val.val.b ? 1 : 0;
        ESP_LOGI(TAG, "Switched to %s", s_active ? "gif_b.raw" : "gif_a.raw");
        esp_rmaker_param_update_and_report(param, val);
    }
    return ESP_OK;
}

static void rainmaker_init(void)
{
    esp_rmaker_config_t cfg = { .enable_time_sync = false };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&cfg, "GIF Display", "GIF Viewer");

    esp_rmaker_device_t *dev = esp_rmaker_device_create(
        "Does this make sense?", ESP_RMAKER_DEVICE_SWITCH, NULL);
    esp_rmaker_device_add_cb(dev, rm_write_cb, NULL);

    esp_rmaker_param_t *p = esp_rmaker_param_create(
        "Answer", ESP_RMAKER_PARAM_TOGGLE,
        esp_rmaker_bool(false), PROP_FLAG_READ | PROP_FLAG_WRITE);
    esp_rmaker_param_add_ui_type(p, ESP_RMAKER_UI_TOGGLE);
    esp_rmaker_device_add_param(dev, p);
    esp_rmaker_device_assign_primary_param(dev, p);
    esp_rmaker_node_add_device(node, dev);

    esp_rmaker_ota_enable_default();
    esp_rmaker_schedule_enable();
    esp_rmaker_scenes_enable();
    esp_rmaker_start();
    ESP_LOGI(TAG, "Rainmaker started");
}

/* ════════════════════════════════════════════════════════
   app_main
   ════════════════════════════════════════════════════════ */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    lcd_init();
    lcd_fill(0x001F);

    sdcard_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    rainmaker_init();
    wifi_prov_init();

    xTaskCreatePinnedToCore(anim_task, "anim", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "Boot complete.");
}
