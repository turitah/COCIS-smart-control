/*
 * ============================================================================
 *  CoCIS SMART CONTROL AND MONITORING SYSTEM
 *  ESP32 + Wokwi  |  Main entrance (entry/exit) + Lecture Room 1 + Room 2
 * ============================================================================
 *
 *  Requirement coverage
 *  --------------------------------------------------------------------------
 *  2.1 Access control / occupancy .... GATE terminal: two MFRC522 readers,
 *                                      per-user inside/outside state, records.
 *  2.2 Automated lighting ............ ROOM1 / ROOM2 terminals: PIR -> light
 *                                      with an empty-room timeout.
 *  2.3 Distributed control ........... Three logical terminals (GATE, ROOM1,
 *                                      ROOM2) coordinated by FreeRTOS queues.
 *                                      Each task is written so it can be moved
 *                                      to a separate ESP32 over UART unchanged.
 *  2.4 Data storage .................. SD card (shared SPI bus) JSON event log
 *                                      + access_records.csv.
 *  2.5 Serial interface .............. Line-based console with status, users,
 *                                      records, schedule, RFID and admin cmds.
 *  2.6 Configurable access schedule .. Time window persisted in NVS, enforced
 *                                      on entry AND exit (exit can be opened
 *                                      by an authenticated administrator).
 *  2.7 Synchronisation ............... One mutex protects all shared state;
 *                                      queues carry events between terminals;
 *                                      a second mutex serialises the SD card.
 *
 *  Shared SPI2 bus: SCK=18 MOSI=23 MISO=19
 *      SD card CS  = 5
 *      RFID entry  = 16
 *      RFID exit   = 17
 *  RST of both MFRC522 modules is tied to 3.3 V (soft reset is used).
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "SMART_COCIS";

/* ==========================================================================
 * PINS / TUNING
 * ========================================================================== */

/* SD card + shared SPI2 bus */
#define PIN_NUM_MISO       19
#define PIN_NUM_MOSI       23
#define PIN_NUM_CLK        18
#define PIN_NUM_CS          5

/* MFRC522 chip-selects (RST tied high) */
#define PIN_RFID_ENTRY_CS  16
#define PIN_RFID_EXIT_CS   17

/* Room 1 terminal */
#define R1_LED_PIN         GPIO_NUM_21
#define R1_PIR_PIN         GPIO_NUM_22
#define R1_TIMEOUT_MS      10000

/* Room 2 terminal */
#define R2_LIGHT_PIN       GPIO_NUM_33
#define R2_PIR_PIN         GPIO_NUM_4
#define R2_TIMEOUT_MS      10000

/* Gate terminal */
#define GREEN_LED          GPIO_NUM_32
#define RED_LED            GPIO_NUM_27
#define BUZZER             GPIO_NUM_14
#define ENTRY_SERVO        GPIO_NUM_25
#define EXIT_SERVO         GPIO_NUM_26
#define UART_PORT          UART_NUM_0

#define SD_MOUNT_POINT     "/sdcard"
#define SD_LOG_FILE        SD_MOUNT_POINT "/system_logs.txt"
#define SD_CSV_FILE        SD_MOUNT_POINT "/access_records.csv"
#define SD_INIT_RETRIES    3
#define SD_RETRY_DELAY_MS  250

#define JSON_EVENT_SIZE    384
#define EVENT_QUEUE_LEN    12
#define GATE_QUEUE_LEN     8

#define ACCESS_RECORD_CAPACITY 128
#define RFID_POLL_INTERVAL_MS  120
#define RFID_REMOVE_MS         1500

#define AUTH_SESSION_MS    (5 * 60 * 1000)
#define AUTH_MAX_FAILURES  3
#define AUTH_LOCKOUT_MS    (30 * 1000)

/* ==========================================================================
 * SHARED TYPES
 * ========================================================================== */

typedef struct {
    char uid_hex[20];       /* "01020304" or "04112233445566" */
    char name[28];
    char role[12];
    bool inside;
} user_t;

/* Persisted in NVS. Plain bytes (no compiler padding surprises). */
typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint8_t start_hour;
    uint8_t start_min;
    uint8_t end_hour;
    uint8_t end_min;
    uint8_t allow_exit_outside;
} schedule_config_t;

#define SCHEDULE_CONFIG_VERSION 1

typedef struct {
    uint32_t seq;
    uint32_t time_s;        /* seconds since midnight */
    char uid[20];
    char name[28];
    char reader[8];         /* ENTRY / EXIT */
    char action[16];        /* ENTRY / EXIT / DENIED_* */
    bool granted;
    char reason[28];
} access_record_t;

typedef struct {
    int occupancy;
    bool room1_occupied;
    bool room1_light;
    bool room2_occupied;
    bool room2_light;
    char last_terminal[12];
    char last_event[20];
    char last_name[28];
    char last_uid[20];
    uint32_t last_time_s;
} system_state_t;

typedef struct {
    char reader[8];
    char uid[20];
} gate_request_t;

/* ==========================================================================
 * GLOBAL STATE
 * ========================================================================== */

/* Terminal <-> terminal messaging */
static QueueHandle_t g_event_queue;     /* JSON strings -> logger task */
static QueueHandle_t g_gate_queue;      /* gate_request_t -> gate task */

/* Shared-resource locks (requirement 2.7) */
static SemaphoreHandle_t g_state_mutex; /* users, occupancy, records, schedule, g_state */
static SemaphoreHandle_t g_sd_mutex;    /* FAT/SPI access to the SD card */
static SemaphoreHandle_t g_rfid_mutex;  /* one SPI conversation at a time */
static volatile bool g_rfid_scan_active;

static bool g_sd_ready;

static system_state_t g_state;
static int g_occupancy;

static user_t g_users[] = {
    {"01020304",       "Dr. Ritah",     "LECTURER", false},
    {"11223344",       "John Student",  "STUDENT",  false},
    {"55667788",       "System Admin",  "ADMIN",    false},
    {"C0FFEE99",       "Grace Warden",  "STAFF",    false},
    {"04112233445566", "Visitor Pass",  "VISITOR",  false}
};
#define USER_COUNT ((int)(sizeof(g_users) / sizeof(g_users[0])))

static access_record_t g_records[ACCESS_RECORD_CAPACITY];
static int g_record_count;
static int g_record_head;
static uint32_t g_record_seq;

static schedule_config_t g_schedule;

/* Software clock (no RTC on the Wokwi board); seeded to 09:00 at boot. */
static volatile uint32_t g_clock_s = 9 * 3600;

/* Console-only authentication state. */
static bool g_auth_active;
static int64_t g_auth_expiry_ms;
static int g_auth_failures;
static int64_t g_auth_lockout_ms;
static char g_admin_pin[8] = "2580";

/* ==========================================================================
 * SMALL HELPERS
 * ========================================================================== */

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void copy_str(char *dst, size_t dst_len, const char *src) {
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static bool eq_ci(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void format_hms(uint32_t seconds, char *out, size_t out_len) {
    seconds %= 86400;
    snprintf(out, out_len, "%02u:%02u:%02u",
             (unsigned)(seconds / 3600),
             (unsigned)((seconds / 60) % 60),
             (unsigned)(seconds % 60));
}

static void uid_to_hex(const uint8_t *uid, int uid_len, char *out, size_t out_len) {
    size_t pos = 0;
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    for (int i = 0; i < uid_len && pos + 2 < out_len; i++) {
        snprintf(out + pos, out_len - pos, "%02X", uid[i]);
        pos += 2;
    }
    out[pos] = '\0';
}

/* Accepts 01:02:03:04, 01-02-03-04 or 01020304 (4 or 7 byte UIDs). */
static bool normalize_uid(const char *input, char *out, size_t out_len) {
    size_t n = 0;
    if (input == NULL || out == NULL || out_len < 15) {
        return false;
    }
    for (const char *p = input; *p != '\0' && n + 1 < out_len; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        out[n++] = (char)toupper((unsigned char)*p);
    }
    out[n] = '\0';
    return n == 8 || n == 14;
}

static bool parse_int(const char *s, int *out) {
    char *end = NULL;
    long value;
    if (s == NULL || out == NULL) {
        return false;
    }
    value = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parse_hhmm(const char *s, int *hour, int *minute) {
    int h = 0;
    int m = 0;
    char extra = 0;
    if (s == NULL || sscanf(s, "%d:%d%c", &h, &m, &extra) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    *hour = h;
    *minute = m;
    return true;
}

static bool parse_hhmmss(const char *s, int *hour, int *minute, int *second) {
    int h = 0;
    int m = 0;
    int sec = 0;
    char extra = 0;
    int fields;
    if (s == NULL) {
        return false;
    }
    fields = sscanf(s, "%d:%d:%d%c", &h, &m, &sec, &extra);
    if (fields == 2) {
        sec = 0;
    } else if (fields != 3) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) {
        return false;
    }
    *hour = h;
    *minute = m;
    *second = sec;
    return true;
}

/* ==========================================================================
 * SCHEDULE (must be called with g_state_mutex held)
 * ========================================================================== */

static void schedule_defaults(void) {
    memset(&g_schedule, 0, sizeof(g_schedule));
    g_schedule.version = SCHEDULE_CONFIG_VERSION;
    g_schedule.enabled = 1;
    g_schedule.start_hour = 8;
    g_schedule.start_min = 0;
    g_schedule.end_hour = 17;
    g_schedule.end_min = 0;
    g_schedule.allow_exit_outside = 0;
}

static bool schedule_is_open_locked(uint32_t now_s) {
    uint32_t start;
    uint32_t end;
    if (!g_schedule.enabled) {
        return true;
    }
    start = (uint32_t)g_schedule.start_hour * 3600U + (uint32_t)g_schedule.start_min * 60U;
    end = (uint32_t)g_schedule.end_hour * 3600U + (uint32_t)g_schedule.end_min * 60U;
    if (start == end) {
        return true; /* open all day */
    }
    if (start < end) {
        return now_s >= start && now_s < end;
    }
    /* Window crosses midnight, e.g. 22:00 - 05:00. */
    return now_s >= start || now_s < end;
}

/* ==========================================================================
 * NVS CONFIGURATION
 * ========================================================================== */

static esp_err_t nvs_save_schedule(const schedule_config_t *cfg) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("cocis", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, "schedule", cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_save_pin(const char *pin) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("cocis", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, "adminpin", pin);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void config_load(void) {
    nvs_handle_t handle;
    schedule_config_t tmp;

    schedule_defaults();

    if (nvs_open("cocis", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    size_t len = sizeof(tmp);
    memset(&tmp, 0, sizeof(tmp));
    if (nvs_get_blob(handle, "schedule", &tmp, &len) == ESP_OK &&
        len == sizeof(tmp) &&
        tmp.version == SCHEDULE_CONFIG_VERSION &&
        tmp.start_hour < 24 && tmp.start_min < 60 &&
        tmp.end_hour < 24 && tmp.end_min < 60) {
        g_schedule = tmp;
    }

    char pin[8];
    len = sizeof(pin);
    if (nvs_get_str(handle, "adminpin", pin, &len) == ESP_OK) {
        size_t pin_len = strlen(pin);
        if (pin_len >= 4 && pin_len <= 7) {
            copy_str(g_admin_pin, sizeof(g_admin_pin), pin);
        }
    }

    nvs_close(handle);
}

/* ==========================================================================
 * EVENT QUEUE / SD LOGGING
 * ========================================================================== */

static void log_line(const char *json) {
    char buffer[JSON_EVENT_SIZE];
    if (g_event_queue == NULL || json == NULL) {
        return;
    }
    copy_str(buffer, sizeof(buffer), json);
    if (xQueueSend(g_event_queue, buffer, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full; event dropped");
    }
}

static void log_access_csv(const access_record_t *record) {
    char time_text[12];
    if (!g_sd_ready || record == NULL) {
        return;
    }
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    FILE *file = fopen(SD_CSV_FILE, "a");
    if (file != NULL) {
        format_hms(record->time_s, time_text, sizeof(time_text));
        fprintf(file, "%lu,%s,%s,%s,%s,%s,%s,%s\n",
                (unsigned long)record->seq, time_text, record->uid, record->name,
                record->reader, record->action,
                record->granted ? "GRANTED" : "DENIED", record->reason);
        fflush(file);
        fclose(file);
    } else {
        ESP_LOGE(TAG, "Cannot open %s", SD_CSV_FILE);
    }
    xSemaphoreGive(g_sd_mutex);
}

static void logger_task(void *arg) {
    (void)arg;
    char line[JSON_EVENT_SIZE];

    while (1) {
        if (xQueueReceive(g_event_queue, line, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!g_sd_ready) {
            continue;
        }
        if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            continue;
        }
        FILE *file = fopen(SD_LOG_FILE, "a");
        if (file != NULL) {
            fprintf(file, "%s\n", line);
            fflush(file);
            fclose(file);
        } else {
            ESP_LOGE(TAG, "Cannot open %s", SD_LOG_FILE);
        }
        xSemaphoreGive(g_sd_mutex);
    }
}

/* ==========================================================================
 * SD CARD (shared SPI2 bus)
 * ========================================================================== */

static esp_err_t sd_mount_once(void) {
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card = NULL;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK; /* already initialised */
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI bus init failed: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }

    ESP_LOGI(TAG, "Mounting SD card: SCK=%d MOSI=%d MISO=%d CS=%d",
             PIN_NUM_CLK, PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CS);

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (0x%x)", esp_err_to_name(ret), ret);
        if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "SD card did not answer. Check 3.3V/GND and "
                          "SCK=18 MOSI=23 MISO=19 CS=5.");
        }
        spi_bus_free(host.slot);
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, card);

    FILE *file = fopen(SD_LOG_FILE, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Mounted, but %s cannot be opened", SD_LOG_FILE);
    } else {
        int written = fprintf(file, "%lld SD_CARD_READY\n",
                              (long long)now_ms());
        int flushed = fflush(file);
        int closed = fclose(file);
        if (written >= 0 && flushed == 0 && closed == 0) {
            ESP_LOGI(TAG, "SD write test passed");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "SD write test failed");
    }

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card);
    spi_bus_free(host.slot);
    return ESP_FAIL;
}

static esp_err_t init_sd_card(void) {
    for (int attempt = 1; attempt <= SD_INIT_RETRIES; attempt++) {
        esp_err_t ret = sd_mount_once();
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD init attempt %d/%d failed", attempt, SD_INIT_RETRIES);
        if (attempt < SD_INIT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(SD_RETRY_DELAY_MS));
        }
    }
    return ESP_FAIL;
}

static void sd_prepare_files(void) {
    if (!g_sd_ready || xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    FILE *file = fopen(SD_CSV_FILE, "r");
    if (file == NULL) {
        file = fopen(SD_CSV_FILE, "a");
        if (file != NULL) {
            fprintf(file, "seq,time,uid,name,reader,action,result,reason\n");
            fflush(file);
            fclose(file);
            ESP_LOGI(TAG, "Created %s", SD_CSV_FILE);
        }
    } else {
        fclose(file);
    }
    xSemaphoreGive(g_sd_mutex);
}

/* ==========================================================================
 * MFRC522 MINIMAL SPI DRIVER
 * Two readers share the SD card's SPI2 bus. Each transaction toggles the
 * selected reader's CS line, so the bus can be used concurrently by the SD
 * card, the entry reader and the exit reader.
 * ========================================================================== */

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t cs;
    const char *label;
    bool ready;
    uint8_t version;
} mfrc522_t;

static mfrc522_t g_rfid[2] = {
    { .spi = NULL, .cs = PIN_RFID_ENTRY_CS, .label = "ENTRY", .ready = false, .version = 0 },
    { .spi = NULL, .cs = PIN_RFID_EXIT_CS,  .label = "EXIT",  .ready = false, .version = 0 }
};

static esp_err_t rfid_spi_bus_init(void) {
    static bool bus_ready = false;

    if (bus_ready) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK; /* SD card already initialised this bus */
    }
    if (ret == ESP_OK) {
        bus_ready = true;
    }
    return ret;
}

/*
 * The MFRC522 uses a half-duplex SPI framing:
 *   bit 0     : 0 = write, 1 = read
 *   bits 1..6 : register address
 *   bit 7     : dummy bit inserted between address and data phases
 * This matches the reference esp-idf-rc522 driver and is required by Wokwi.
 */
static esp_err_t mfrc522_reg_write(mfrc522_t *dev, uint8_t reg, uint8_t value) {
    spi_transaction_t t = {0};
    t.flags = SPI_TRANS_USE_TXDATA;
    t.cmd = 0;
    t.addr = (uint16_t)(reg & 0x3F);
    t.length = 8;
    t.tx_data[0] = value;
    return spi_device_polling_transmit(dev->spi, &t);
}

static esp_err_t mfrc522_reg_read(mfrc522_t *dev, uint8_t reg, uint8_t *value) {
    spi_transaction_t t = {0};
    esp_err_t ret;
    t.flags = SPI_TRANS_USE_RXDATA;
    t.cmd = 1;
    t.addr = (uint16_t)(reg & 0x3F);
    t.length = 0;
    t.rxlength = 8;
    ret = spi_device_polling_transmit(dev->spi, &t);
    if (ret == ESP_OK && value != NULL) {
        *value = t.rx_data[0];
    }
    return ret;
}

/* CRC_A used by ISO14443-A SELECT frames. */
static uint16_t crc_a(const uint8_t *data, size_t len) {
    uint16_t crc = 0x6363;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = (uint8_t)(data[i] ^ (crc & 0xFF));
        ch = (uint8_t)(ch ^ (ch << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)ch << 8) ^
                         ((uint16_t)ch << 3) ^ ((uint16_t)ch >> 4));
    }
    return crc;
}

/*
 * Sends one frame and waits for the receiver interrupt.
 * valid_bits != 0 is used for short frames such as REQA (7 bits).
 */
static bool mfrc522_transceive(mfrc522_t *dev, const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_max, size_t *rx_len,
                               uint8_t valid_bits) {
    uint8_t irq = 0;
    uint8_t error = 0;
    uint8_t level = 0;
    bool finished = false;

    if (dev == NULL || !dev->ready || tx == NULL) {
        return false;
    }

    mfrc522_reg_write(dev, 0x01, 0x00);      /* CommandReg = Idle */
    mfrc522_reg_write(dev, 0x04, 0x7F);      /* clear ComIrqReg */
    mfrc522_reg_write(dev, 0x0A, 0x80);      /* flush FIFO */

    for (size_t i = 0; i < tx_len; i++) {
        mfrc522_reg_write(dev, 0x09, tx[i]);
    }

    mfrc522_reg_write(dev, 0x0D, valid_bits);        /* BitFramingReg */
    mfrc522_reg_write(dev, 0x01, 0x0C);              /* Transceive */
    mfrc522_reg_write(dev, 0x0D, valid_bits | 0x80); /* StartSend */

    for (int i = 0; i < 400; i++) {
        if (mfrc522_reg_read(dev, 0x04, &irq) != ESP_OK) {
            return false;
        }
        if ((irq & 0x21) != 0) { /* RxIRq or TimerIRq */
            finished = true;
            break;
        }
        esp_rom_delay_us(100); /* up to ~40 ms total */
    }

    if (!finished || (irq & 0x20) == 0) {
        return false; /* no answer */
    }

    mfrc522_reg_read(dev, 0x06, &error);
    if ((error & 0x1B) != 0) { /* Protocol/Parity/Collision/Buffer overflow */
        return false;
    }

    mfrc522_reg_read(dev, 0x0A, &level);
    size_t count = level < rx_max ? level : rx_max;
    for (size_t i = 0; i < count; i++) {
        uint8_t byte = 0;
        if (mfrc522_reg_read(dev, 0x09, &byte) != ESP_OK) {
            return false;
        }
        if (rx != NULL) {
            rx[i] = byte;
        }
    }
    if (rx_len != NULL) {
        *rx_len = count;
    }
    return true;
}

static bool mfrc522_select(mfrc522_t *dev, uint8_t sel, const uint8_t *uid4,
                            uint8_t *sak) {
    uint8_t frame[9];
    uint8_t response[3];
    size_t response_len = 0;
    uint16_t crc;

    frame[0] = sel;
    frame[1] = 0x70;
    memcpy(frame + 2, uid4, 4);
    frame[6] = (uint8_t)(uid4[0] ^ uid4[1] ^ uid4[2] ^ uid4[3]);
    crc = crc_a(frame, 7);
    frame[7] = (uint8_t)(crc & 0xFF);
    frame[8] = (uint8_t)(crc >> 8);

    if (!mfrc522_transceive(dev, frame, sizeof(frame), response,
                            sizeof(response), &response_len, 0)) {
        return false;
    }
    if (response_len < 1) {
        return false;
    }
    if (sak != NULL) {
        *sak = response[0];
    }
    return true;
}

/*
 * Full ISO14443-A anticollision + cascade levels, so both 4-byte MIFARE
 * Classic cards and 7-byte Ultralight/NTAG presets are supported.
 */
static bool mfrc522_read_uid(mfrc522_t *dev, uint8_t *uid, int *uid_len) {
    uint8_t command = 0x26; /* REQA */
    uint8_t atqa[2];
    uint8_t sel = 0x93;
    int length = 0;

    if (dev == NULL || !dev->ready || uid == NULL || uid_len == NULL) {
        return false;
    }

    size_t atqa_len = 0;
    if (!mfrc522_transceive(dev, &command, 1, atqa, sizeof(atqa),
                            &atqa_len, 7) || atqa_len < 2) {
        return false;
    }

    for (int level = 0; level < 3; level++) {
        uint8_t anticoll[2] = { sel, 0x20 };
        uint8_t frame[5];
        size_t frame_len = 0;
        uint8_t sak = 0;

        if (!mfrc522_transceive(dev, anticoll, sizeof(anticoll), frame,
                                sizeof(frame), &frame_len, 0) || frame_len < 5) {
            return false;
        }
        if ((frame[0] ^ frame[1] ^ frame[2] ^ frame[3] ^ frame[4]) != 0) {
            return false; /* BCC error */
        }

        if (frame[0] == 0x88) {
            /* Cascade tag: 3 UID bytes at this level, then continue. */
            uint8_t cascade_uid[4] = { 0x88, frame[1], frame[2], frame[3] };
            if (!mfrc522_select(dev, sel, cascade_uid, &sak)) {
                return false;
            }
            if (length + 3 > 10) {
                return false;
            }
            memcpy(uid + length, frame + 1, 3);
            length += 3;
            sel = (sel == 0x93) ? 0x95 : 0x97;
            continue;
        }

        /* Final cascade level: 4 UID bytes. */
        uint8_t final_uid[4] = { frame[0], frame[1], frame[2], frame[3] };
        if (!mfrc522_select(dev, sel, final_uid, &sak)) {
            return false;
        }
        if (length + 4 > 10) {
            return false;
        }
        memcpy(uid + length, final_uid, 4);
        length += 4;
        *uid_len = length;
        return length == 4 || length == 7 || length == 10;
    }
    return false;
}

static bool mfrc522_init(mfrc522_t *dev) {
    esp_err_t ret;

    if (dev == NULL) {
        return false;
    }

    if (dev->spi == NULL) {
        ret = rfid_spi_bus_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RFID %s: SPI bus init failed: %s",
                     dev->label, esp_err_to_name(ret));
            return false;
        }
        spi_device_interface_config_t config = {
            .clock_speed_hz = 1000000,
            .mode = 0,
            .spics_io_num = dev->cs,
            .queue_size = 1,
            .command_bits = 1,
            .address_bits = 6,
            .dummy_bits = 1,
            .flags = SPI_DEVICE_HALFDUPLEX,
        };
        ret = spi_bus_add_device(SPI2_HOST, &config, &dev->spi);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RFID %s: add device failed: %s",
                     dev->label, esp_err_to_name(ret));
            return false;
        }
    }

    dev->ready = true;

    /* Soft reset, then the register set recommended by the MFRC522 library. */
    mfrc522_reg_write(dev, 0x01, 0x0F);
    vTaskDelay(pdMS_TO_TICKS(50));
    mfrc522_reg_write(dev, 0x12, 0x00); /* TxModeReg */
    mfrc522_reg_write(dev, 0x13, 0x00); /* RxModeReg */
    mfrc522_reg_write(dev, 0x24, 0x26); /* ModWidthReg */
    mfrc522_reg_write(dev, 0x2A, 0x80); /* TModeReg */
    mfrc522_reg_write(dev, 0x2B, 0xA9); /* TPrescalerReg */
    mfrc522_reg_write(dev, 0x2C, 0x03); /* TReloadRegH */
    mfrc522_reg_write(dev, 0x2D, 0xE8); /* TReloadRegL */
    mfrc522_reg_write(dev, 0x15, 0x40); /* TxASKReg */
    mfrc522_reg_write(dev, 0x11, 0x3D); /* ModeReg */

    uint8_t tx_control = 0;
    mfrc522_reg_read(dev, 0x14, &tx_control);
    if ((tx_control & 0x03) != 0x03) {
        mfrc522_reg_write(dev, 0x14, (uint8_t)(tx_control | 0x03));
    }

    /* Verify communication with a read-back of a register we just wrote. */
    uint8_t mode_check = 0;
    mfrc522_reg_read(dev, 0x11, &mode_check);
    mfrc522_reg_read(dev, 0x37, &dev->version);

    if (mode_check != 0x3D) {
        dev->ready = false;
        ESP_LOGE(TAG, "RFID %s: SPI communication failure "
                      "(CS=GPIO%d version=0x%02X ModeReg=0x%02X, expected 0x3D)",
                 dev->label, dev->cs, dev->version, mode_check);
        return false;
    }

    ESP_LOGI(TAG, "RFID %s ready: CS=GPIO%d version=0x%02X ModeReg=0x%02X",
             dev->label, dev->cs, dev->version, mode_check);
    if (dev->version == 0x00 || dev->version == 0xFF) {
        ESP_LOGW(TAG, "RFID %s: VersionReg=0x%02X not reported by this "
                      "simulator; register access works, polling anyway",
                 dev->label, dev->version);
    }
    return true;
}

/* ==========================================================================
 * GATE TERMINAL: state transitions, records, feedback
 * ========================================================================== */

static int find_user_locked(const char *uid) {
    for (int i = 0; i < USER_COUNT; i++) {
        if (strcmp(g_users[i].uid_hex, uid) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_record_locked(const access_record_t *record) {
    g_records[g_record_head] = *record;
    g_record_head = (g_record_head + 1) % ACCESS_RECORD_CAPACITY;
    if (g_record_count < ACCESS_RECORD_CAPACITY) {
        g_record_count++;
    }
}

static uint32_t servo_duty(uint32_t pulse_us) {
    return (pulse_us * 65535U) / 20000U;
}

static void set_servo(ledc_channel_t channel, uint32_t pulse_us) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, servo_duty(pulse_us));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

static void open_entry_gate(void) {
    set_servo(LEDC_CHANNEL_0, 2000);
    vTaskDelay(pdMS_TO_TICKS(2000));
    set_servo(LEDC_CHANNEL_0, 1000);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void open_exit_gate(void) {
    set_servo(LEDC_CHANNEL_1, 2000);
    vTaskDelay(pdMS_TO_TICKS(2000));
    set_servo(LEDC_CHANNEL_1, 1000);
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void beep_once(void) {
    gpio_set_level(BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(BUZZER, 0);
}

static void beep_denied(void) {
    for (int i = 0; i < 3; i++) {
        gpio_set_level(BUZZER, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(BUZZER, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static void gate_feedback(bool granted, bool entry) {
    if (granted) {
        gpio_set_level(GREEN_LED, 1);
        beep_once();
        if (entry) {
            open_entry_gate();
        } else {
            open_exit_gate();
        }
        gpio_set_level(GREEN_LED, 0);
    } else {
        gpio_set_level(RED_LED, 1);
        beep_denied();
        gpio_set_level(RED_LED, 0);
    }
}

/*
 * The single place where a card presentation changes shared state. Everything
 * that reads or writes users/occupancy/records/schedule does so under
 * g_state_mutex, which prevents lost updates and inconsistent records when the
 * gate and the two room terminals (or several future MCUs) act concurrently.
 */
static void handle_gate_request(const gate_request_t *request) {
    bool entry = eq_ci(request->reader, "ENTRY");
    bool granted = false;
    char reason[28] = "UNKNOWN_CARD";
    char name[28] = "UNKNOWN";
    char event[20];
    char json[JSON_EVENT_SIZE];
    char time_text[12];
    access_record_t record;
    int occupancy_snapshot;

    memset(&record, 0, sizeof(record));

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    uint32_t now_s = g_clock_s;
    bool schedule_open = schedule_is_open_locked(now_s);
    int user_index = find_user_locked(request->uid);

    if (user_index >= 0) {
        copy_str(name, sizeof(name), g_users[user_index].name);

        if (!schedule_open) {
            if (entry) {
                copy_str(reason, sizeof(reason), "SCHEDULE_CLOSED");
            } else if (!g_schedule.allow_exit_outside) {
                copy_str(reason, sizeof(reason), "EXIT_OUTSIDE_SCHEDULE");
            } else {
                reason[0] = '\0';
            }
        } else {
            reason[0] = '\0';
        }

        if (reason[0] == '\0') {
            if (entry && g_users[user_index].inside) {
                copy_str(reason, sizeof(reason), "ALREADY_INSIDE");
            } else if (!entry && !g_users[user_index].inside) {
                copy_str(reason, sizeof(reason), "NOT_INSIDE");
            }
        }

        if (reason[0] == '\0') {
            granted = true;
            if (entry) {
                g_users[user_index].inside = true;
                g_occupancy++;
            } else {
                g_users[user_index].inside = false;
                g_occupancy--;
            }
            copy_str(reason, sizeof(reason), "OK");
        }
    }

    if (granted) {
        copy_str(event, sizeof(event), entry ? "ENTRY" : "EXIT");
    } else {
        copy_str(event, sizeof(event), entry ? "DENIED_ENTRY" : "DENIED_EXIT");
    }

    g_state.occupancy = g_occupancy;
    copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "GATE");
    copy_str(g_state.last_event, sizeof(g_state.last_event), event);
    copy_str(g_state.last_name, sizeof(g_state.last_name), name);
    copy_str(g_state.last_uid, sizeof(g_state.last_uid), request->uid);
    g_state.last_time_s = now_s;
    occupancy_snapshot = g_occupancy;

    record.seq = ++g_record_seq;
    record.time_s = now_s;
    copy_str(record.uid, sizeof(record.uid), request->uid);
    copy_str(record.name, sizeof(record.name), name);
    copy_str(record.reader, sizeof(record.reader), request->reader);
    copy_str(record.action, sizeof(record.action), event);
    record.granted = granted;
    copy_str(record.reason, sizeof(record.reason), reason);
    add_record_locked(&record);

    format_hms(now_s, time_text, sizeof(time_text));
    snprintf(json, sizeof(json),
             "{\"terminal\":\"GATE\",\"event\":\"%s\",\"reader\":\"%s\","
             "\"uid\":\"%s\",\"name\":\"%s\",\"granted\":%s,\"reason\":\"%s\","
             "\"occupancy\":%d,\"time\":\"%s\"}",
             event, request->reader, request->uid, name,
             granted ? "true" : "false", reason, occupancy_snapshot, time_text);

    xSemaphoreGive(g_state_mutex);

    ESP_LOGI(TAG, "%s %s uid=%s name=%s reason=%s occupancy=%d",
             event, request->reader, request->uid, name, reason,
             occupancy_snapshot);

    log_line(json);
    log_access_csv(&record);
    gate_feedback(granted, entry);
}

static void gate_action_task(void *arg) {
    (void)arg;
    gate_request_t request;
    while (1) {
        if (xQueueReceive(g_gate_queue, &request, portMAX_DELAY) == pdTRUE) {
            handle_gate_request(&request);
        }
    }
}

/* ==========================================================================
 * GATE TERMINAL: MFRC522 polling
 * ========================================================================== */

static void rfid_reader_task(void *arg) {
    (void)arg;
    bool present[2] = { false, false };
    char last_uid[2][20] = { { 0 }, { 0 } };
    int64_t last_seen[2] = { 0, 0 };
    int index = 0;

    while (1) {
        mfrc522_t *reader = &g_rfid[index];
        if (reader->ready && !g_rfid_scan_active) {
            uint8_t uid[10];
            int uid_len = 0;
            bool got_card;

            xSemaphoreTake(g_rfid_mutex, portMAX_DELAY);
            got_card = mfrc522_read_uid(reader, uid, &uid_len);
            xSemaphoreGive(g_rfid_mutex);

            if (got_card) {
                char hex[24];
                int64_t now = now_ms();
                uid_to_hex(uid, uid_len, hex, sizeof(hex));

                if (!present[index] || strcmp(hex, last_uid[index]) != 0) {
                    gate_request_t request;
                    memset(&request, 0, sizeof(request));
                    copy_str(request.reader, sizeof(request.reader), reader->label);
                    copy_str(request.uid, sizeof(request.uid), hex);
                    if (xQueueSend(g_gate_queue, &request, pdMS_TO_TICKS(100)) != pdTRUE) {
                        ESP_LOGW(TAG, "Gate queue full; %s card %s dropped",
                                 reader->label, hex);
                    } else {
                        ESP_LOGI(TAG, "Card %s presented at %s", hex, reader->label);
                    }
                    present[index] = true;
                    copy_str(last_uid[index], sizeof(last_uid[index]), hex);
                }
                last_seen[index] = now;
            } else if (present[index] && (now_ms() - last_seen[index]) > RFID_REMOVE_MS) {
                present[index] = false;
                last_uid[index][0] = '\0';
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RFID_POLL_INTERVAL_MS));
        index ^= 1;
    }
}

/* ==========================================================================
 * ROOM TERMINALS: PIR -> light, with an empty-room timeout
 * ========================================================================== */

static void log_room_event(const char *terminal, bool occupied, bool light_on,
                           int sequence, uint32_t now_s) {
    char json[JSON_EVENT_SIZE];
    char time_text[12];
    format_hms(now_s, time_text, sizeof(time_text));
    snprintf(json, sizeof(json),
             "{\"terminal\":\"%s\",\"event\":\"%s\",\"occupied\":%s,"
             "\"light_on\":%s,\"sequence\":%d,\"time\":\"%s\"}",
             terminal, occupied ? "OCCUPIED" : "EMPTY",
             occupied ? "true" : "false", light_on ? "true" : "false",
             sequence, time_text);
    ESP_LOGI(TAG, "%s %s light=%s seq=%d", terminal,
             occupied ? "OCCUPIED" : "EMPTY", light_on ? "ON" : "OFF", sequence);
    log_line(json);
}

static void task_room1(void *arg) {
    (void)arg;
    bool occupied = false;
    int64_t last_motion_ms = 0;
    int sequence = 0;

    while (1) {
        int64_t now = now_ms();
        bool motion = gpio_get_level(R1_PIR_PIN) != 0;

        if (motion) {
            last_motion_ms = now;
            if (!occupied) {
                occupied = true;
                gpio_set_level(R1_LED_PIN, 1);
                sequence++;

                uint32_t now_s = g_clock_s;
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                g_state.room1_occupied = true;
                g_state.room1_light = true;
                copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "ROOM1");
                copy_str(g_state.last_event, sizeof(g_state.last_event), "OCCUPIED");
                g_state.last_time_s = now_s;
                xSemaphoreGive(g_state_mutex);

                log_room_event("ROOM1", true, true, sequence, now_s);
            }
        } else if (occupied && (now - last_motion_ms) > R1_TIMEOUT_MS) {
            occupied = false;
            gpio_set_level(R1_LED_PIN, 0);
            sequence++;

            uint32_t now_s = g_clock_s;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_state.room1_occupied = false;
            g_state.room1_light = false;
            copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "ROOM1");
            copy_str(g_state.last_event, sizeof(g_state.last_event), "EMPTY");
            g_state.last_time_s = now_s;
            xSemaphoreGive(g_state_mutex);

            log_room_event("ROOM1", false, false, sequence, now_s);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void task_room2(void *arg) {
    (void)arg;
    bool occupied = false;
    int64_t last_motion_ms = 0;
    int sequence = 0;

    while (1) {
        int64_t now = now_ms();
        bool motion = gpio_get_level(R2_PIR_PIN) != 0;

        if (motion) {
            last_motion_ms = now;
            if (!occupied) {
                occupied = true;
                gpio_set_level(R2_LIGHT_PIN, 1);
                sequence++;

                uint32_t now_s = g_clock_s;
                xSemaphoreTake(g_state_mutex, portMAX_DELAY);
                g_state.room2_occupied = true;
                g_state.room2_light = true;
                copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "ROOM2");
                copy_str(g_state.last_event, sizeof(g_state.last_event), "OCCUPIED");
                g_state.last_time_s = now_s;
                xSemaphoreGive(g_state_mutex);

                log_room_event("ROOM2", true, true, sequence, now_s);
            }
        } else if (occupied && (now - last_motion_ms) > R2_TIMEOUT_MS) {
            occupied = false;
            gpio_set_level(R2_LIGHT_PIN, 0);
            sequence++;

            uint32_t now_s = g_clock_s;
            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            g_state.room2_occupied = false;
            g_state.room2_light = false;
            copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "ROOM2");
            copy_str(g_state.last_event, sizeof(g_state.last_event), "EMPTY");
            g_state.last_time_s = now_s;
            xSemaphoreGive(g_state_mutex);

            log_room_event("ROOM2", false, false, sequence, now_s);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ==========================================================================
 * SERIAL CONSOLE (requirement 2.5)
 * ========================================================================== */

static bool is_authed(void) {
    if (g_auth_active && now_ms() < g_auth_expiry_ms) {
        return true;
    }
    g_auth_active = false;
    return false;
}

static bool require_auth(void) {
    if (is_authed()) {
        return true;
    }
    printf("ERROR: administrator authorization required. Use: auth <PIN>\r\n");
    return false;
}

static void print_help(void) {
    printf("\r\n=== CoCIS SMART SYSTEM COMMANDS ===\r\n");
    printf("  help / menu                  This menu\r\n");
    printf("  status                       Full system status\r\n");
    printf("  occupancy                    Building occupancy + who is inside\r\n");
    printf("  rooms                        Room occupancy and light state\r\n");
    printf("  users                        Registered RFID users\r\n");
    printf("  records [n]                  Last n access records (default 10)\r\n");
    printf("  rfid                         RFID reader health\r\n");
    printf("  tap <entry|exit> <UID>       Simulate a card tap (no hardware needed)\r\n");
    printf("  scan [seconds]               Read cards live from both readers\r\n");
    printf("  time                         Show the software clock\r\n");
    printf("  schedule                     Show the access schedule\r\n");
    printf("\r\n-- authenticated configuration --\r\n");
    printf("  auth <PIN>                   Unlock configuration for 5 minutes\r\n");
    printf("  logout                       End the authorized session\r\n");
    printf("  passwd <old> <new>           Change the admin PIN\r\n");
    printf("  schedule set HH:MM HH:MM     Set the access window\r\n");
    printf("  schedule enable|disable      Enable/disable the access window\r\n");
    printf("  schedule exit-outside on|off Allow/block exits outside the window\r\n");
    printf("  time set HH:MM[:SS]          Set the software clock\r\n");
    printf("  records clear                Clear the in-RAM access records\r\n");
    printf("===================================\r\n");
}

static void cmd_status(void) {
    system_state_t state;
    schedule_config_t schedule;
    int occupancy;
    uint32_t now_s;
    bool open;
    char time_text[12];

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    state = g_state;
    schedule = g_schedule;
    occupancy = g_occupancy;
    now_s = g_clock_s;
    open = schedule_is_open_locked(now_s);
    xSemaphoreGive(g_state_mutex);

    format_hms(now_s, time_text, sizeof(time_text));

    printf("\r\n--- SYSTEM STATUS ---\r\n");
    printf("Clock             : %s\r\n", time_text);
    printf("Access schedule   : %s  %02u:%02u-%02u:%02u  (%s)\r\n",
           schedule.enabled ? "ENABLED" : "DISABLED",
           schedule.start_hour, schedule.start_min,
           schedule.end_hour, schedule.end_min,
           schedule.enabled ? (open ? "OPEN" : "CLOSED") : "ALWAYS OPEN");
    printf("Exit outside hours: %s\r\n",
           schedule.allow_exit_outside ? "ALLOWED" : "BLOCKED");
    printf("Building occupancy: %d person(s) inside\r\n", occupancy);
    printf("Room 1            : %s, light %s\r\n",
           state.room1_occupied ? "OCCUPIED" : "EMPTY",
           state.room1_light ? "ON" : "OFF");
    printf("Room 2            : %s, light %s\r\n",
           state.room2_occupied ? "OCCUPIED" : "EMPTY",
           state.room2_light ? "ON" : "OFF");
    printf("SD card           : %s\r\n", g_sd_ready ? "READY" : "NOT READY");
    printf("RFID              : ENTRY %s | EXIT %s\r\n",
           g_rfid[0].ready ? "READY" : "NOT DETECTED",
           g_rfid[1].ready ? "READY" : "NOT DETECTED");
    printf("Last event        : %s %s %s at %s\r\n",
           state.last_terminal, state.last_event,
           state.last_name[0] ? state.last_name : "-",
           state.last_uid[0] ? state.last_uid : "-");
    printf("---------------------\r\n");
}

static void cmd_occupancy(void) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    printf("\r\nBuilding occupancy: %d person(s) inside\r\n", g_occupancy);
    printf("Users currently inside:\r\n");
    bool any = false;
    for (int i = 0; i < USER_COUNT; i++) {
        if (g_users[i].inside) {
            any = true;
            printf("  %-14s %-12s %s\r\n",
                   g_users[i].uid_hex, g_users[i].role, g_users[i].name);
        }
    }
    if (!any) {
        printf("  (nobody)\r\n");
    }
    xSemaphoreGive(g_state_mutex);
}

static void cmd_rooms(void) {
    system_state_t state;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    state = g_state;
    xSemaphoreGive(g_state_mutex);

    printf("\r\nLecture Room 1: %s, light %s\r\n",
           state.room1_occupied ? "OCCUPIED" : "EMPTY",
           state.room1_light ? "ON" : "OFF");
    printf("Lecture Room 2: %s, light %s\r\n",
           state.room2_occupied ? "OCCUPIED" : "EMPTY",
           state.room2_light ? "ON" : "OFF");
}

static void cmd_users(void) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    printf("\r\nRegistered RFID users:\r\n");
    printf("  %-16s %-10s %-16s %s\r\n", "UID", "ROLE", "NAME", "LOCATION");
    for (int i = 0; i < USER_COUNT; i++) {
        printf("  %-16s %-10s %-16s %s\r\n",
               g_users[i].uid_hex, g_users[i].role, g_users[i].name,
               g_users[i].inside ? "INSIDE" : "OUTSIDE");
    }
    xSemaphoreGive(g_state_mutex);
}

static void cmd_records(int limit) {
    int count;
    int head;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    count = g_record_count;
    head = g_record_head;
    xSemaphoreGive(g_state_mutex);

    if (count == 0) {
        printf("\r\nNo access records yet.\r\n");
        return;
    }
    if (limit <= 0 || limit > count) {
        limit = count;
    }

    printf("\r\nLast %d access record(s):\r\n", limit);
    printf("  SEQ   TIME      ACTION        READER  UID              NAME             RESULT   REASON\r\n");

    int start = (head - limit + ACCESS_RECORD_CAPACITY) % ACCESS_RECORD_CAPACITY;
    for (int i = 0; i < limit; i++) {
        access_record_t record;
        int index = (start + i) % ACCESS_RECORD_CAPACITY;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        record = g_records[index];
        xSemaphoreGive(g_state_mutex);

        char time_text[12];
        format_hms(record.time_s, time_text, sizeof(time_text));
        printf("  %04lu  %s  %-12s  %-6s  %-14s  %-15s  %-7s  %s\r\n",
               (unsigned long)record.seq, time_text, record.action, record.reader,
               record.uid, record.name,
               record.granted ? "GRANTED" : "DENIED", record.reason);
    }
}

static void cmd_rfid(void) {
    printf("\r\nMFRC522 readers on the shared SPI2 bus:\r\n");
    for (int i = 0; i < 2; i++) {
        uint8_t mode = 0;
        uint8_t version = 0;
        bool comm_ok = false;

        if (g_rfid[i].spi != NULL) {
            xSemaphoreTake(g_rfid_mutex, portMAX_DELAY);
            comm_ok = (mfrc522_reg_read(&g_rfid[i], 0x11, &mode) == ESP_OK) &&
                      (mfrc522_reg_read(&g_rfid[i], 0x37, &version) == ESP_OK);
            xSemaphoreGive(g_rfid_mutex);
        }
        if (!comm_ok) {
            mode = 0;
            version = g_rfid[i].version;
        }

        printf("  %-5s CS=GPIO%-2d  %-13s version=0x%02X ModeReg=0x%02X %s\r\n",
               g_rfid[i].label, g_rfid[i].cs,
               g_rfid[i].ready ? "READY" : "NOT DETECTED",
               version, mode,
               (comm_ok && mode == 0x3D) ? "SPI OK" : "SPI FAIL");
    }
    printf("Scanning is automatic. Tap a card on a reader.\r\n");
    printf("No reader? Use: tap <entry|exit> <UID>\r\n");
}

/*
 * Active diagnostic scan. The background poller is paused so the two tasks do
 * not interleave register sequences on the same reader.
 */
static void cmd_scan(int seconds) {
    char seen[8][20];
    int seen_count = 0;

    if (seconds <= 0 || seconds > 30) {
        seconds = 5;
    }
    printf("\r\nScanning both readers for %d second(s)...\r\n", seconds);

    g_rfid_scan_active = true;
    vTaskDelay(pdMS_TO_TICKS(300));

    int64_t deadline = now_ms() + (int64_t)seconds * 1000;
    while (now_ms() < deadline) {
        for (int r = 0; r < 2; r++) {
            uint8_t uid[10];
            int uid_len = 0;
            bool got_card;

            if (!g_rfid[r].ready) {
                continue;
            }

            xSemaphoreTake(g_rfid_mutex, portMAX_DELAY);
            got_card = mfrc522_read_uid(&g_rfid[r], uid, &uid_len);
            xSemaphoreGive(g_rfid_mutex);
            if (!got_card) {
                continue;
            }

            char hex[24];
            bool duplicate = false;
            uid_to_hex(uid, uid_len, hex, sizeof(hex));
            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], hex) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (seen_count < (int)(sizeof(seen) / sizeof(seen[0]))) {
                copy_str(seen[seen_count++], sizeof(seen[0]), hex);
            }

            xSemaphoreTake(g_state_mutex, portMAX_DELAY);
            int user_index = find_user_locked(hex);
            printf("  %s reader: %s  %s\r\n", g_rfid[r].label, hex,
                   user_index >= 0 ? g_users[user_index].name : "UNKNOWN CARD");
            xSemaphoreGive(g_state_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    g_rfid_scan_active = false;
    if (seen_count == 0) {
        printf("No card detected. Use 'tap <entry|exit> <UID>' to simulate one.\r\n");
    } else {
        printf("Scan finished: %d card(s) seen.\r\n", seen_count);
    }
}

static void cmd_schedule_show(void) {
    schedule_config_t schedule;
    uint32_t now_s;
    bool open;
    char time_text[12];

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    schedule = g_schedule;
    now_s = g_clock_s;
    open = schedule_is_open_locked(now_s);
    xSemaphoreGive(g_state_mutex);

    format_hms(now_s, time_text, sizeof(time_text));

    printf("\r\nAccess schedule: %s\r\n", schedule.enabled ? "ENABLED" : "DISABLED");
    printf("  Window      : %02u:%02u - %02u:%02u\r\n",
           schedule.start_hour, schedule.start_min,
           schedule.end_hour, schedule.end_min);
    printf("  Clock       : %s\r\n", time_text);
    printf("  Exit outside: %s\r\n",
           schedule.allow_exit_outside ? "ALLOWED" : "BLOCKED");
    if (schedule.enabled) {
        printf("  Status      : %s\r\n", open ? "OPEN" : "CLOSED");
    }
}

static void cmd_schedule_set(const char *start, const char *end) {
    int sh;
    int sm;
    int eh;
    int em;
    schedule_config_t schedule;

    if (!require_auth()) {
        return;
    }
    if (!parse_hhmm(start, &sh, &sm) || !parse_hhmm(end, &eh, &em)) {
        printf("Usage: schedule set HH:MM HH:MM (e.g. schedule set 08:00 17:00)\r\n");
        return;
    }
    if (sh == eh && sm == em) {
        printf("Start and end are equal; the system would be open all day.\r\n");
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    schedule = g_schedule;
    schedule.start_hour = (uint8_t)sh;
    schedule.start_min = (uint8_t)sm;
    schedule.end_hour = (uint8_t)eh;
    schedule.end_min = (uint8_t)em;
    schedule.enabled = 1;
    schedule.version = SCHEDULE_CONFIG_VERSION;
    g_schedule = schedule;
    xSemaphoreGive(g_state_mutex);

    if (nvs_save_schedule(&schedule) == ESP_OK) {
        printf("Schedule updated and saved: %02d:%02d - %02d:%02d\r\n",
               sh, sm, eh, em);
    } else {
        printf("Schedule updated in RAM (NVS save failed).\r\n");
    }
}

static void cmd_schedule_toggle(bool enable) {
    schedule_config_t schedule;
    if (!require_auth()) {
        return;
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    schedule = g_schedule;
    schedule.enabled = enable ? 1 : 0;
    g_schedule = schedule;
    xSemaphoreGive(g_state_mutex);

    if (nvs_save_schedule(&schedule) == ESP_OK) {
        printf("Access schedule %s and saved.\r\n", enable ? "ENABLED" : "DISABLED");
    } else {
        printf("Access schedule %s in RAM (NVS save failed).\r\n",
               enable ? "ENABLED" : "DISABLED");
    }
}

static void cmd_schedule_exit(bool allow) {
    schedule_config_t schedule;
    if (!require_auth()) {
        return;
    }

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    schedule = g_schedule;
    schedule.allow_exit_outside = allow ? 1 : 0;
    g_schedule = schedule;
    xSemaphoreGive(g_state_mutex);

    if (nvs_save_schedule(&schedule) == ESP_OK) {
        printf("Exits outside the access window are now %s and saved.\r\n",
               allow ? "ALLOWED" : "BLOCKED");
    } else {
        printf("Exit policy changed in RAM (NVS save failed).\r\n");
    }
}

static void cmd_time_show(void) {
    uint32_t now_s = g_clock_s;
    char time_text[12];
    format_hms(now_s, time_text, sizeof(time_text));
    printf("\r\nSoftware clock: %s\r\n", time_text);
}

static void cmd_time_set(const char *value) {
    int h;
    int m;
    int s;
    if (!require_auth()) {
        return;
    }
    if (!parse_hhmmss(value, &h, &m, &s)) {
        printf("Usage: time set HH:MM[:SS]\r\n");
        return;
    }
    g_clock_s = (uint32_t)h * 3600U + (uint32_t)m * 60U + (uint32_t)s;
    printf("Clock set to %02d:%02d:%02d.\r\n", h, m, s);
}

static void cmd_tap(const char *side, const char *raw_uid) {
    char uid[20];
    gate_request_t request;

    if (!normalize_uid(raw_uid, uid, sizeof(uid))) {
        printf("Invalid UID. Use 8 hex digits (4 bytes) or 14 hex digits (7 bytes).\r\n");
        return;
    }
    if (!eq_ci(side, "entry") && !eq_ci(side, "exit")) {
        printf("Usage: tap <entry|exit> <UID>\r\n");
        return;
    }

    memset(&request, 0, sizeof(request));
    copy_str(request.reader, sizeof(request.reader), eq_ci(side, "entry") ? "ENTRY" : "EXIT");
    copy_str(request.uid, sizeof(request.uid), uid);

    if (xQueueSend(g_gate_queue, &request, pdMS_TO_TICKS(200)) != pdTRUE) {
        printf("Gate queue is busy; try again.\r\n");
        return;
    }
    printf("Injected %s card %s; watch the gate result.\r\n",
           request.reader, request.uid);
}

static void cmd_auth(const char *pin) {
    int64_t now = now_ms();

    if (g_auth_lockout_ms > now) {
        int remaining = (int)((g_auth_lockout_ms - now + 999) / 1000);
        printf("Too many failed attempts. Try again in %d seconds.\r\n", remaining);
        return;
    }
    if (pin != NULL && strcmp(pin, g_admin_pin) == 0) {
        g_auth_active = true;
        g_auth_expiry_ms = now + AUTH_SESSION_MS;
        g_auth_failures = 0;
        printf("Authorized. Configuration commands unlocked for %d minutes.\r\n",
               AUTH_SESSION_MS / 60000);
        return;
    }

    g_auth_failures++;
    if (g_auth_failures >= AUTH_MAX_FAILURES) {
        g_auth_failures = 0;
        g_auth_lockout_ms = now + AUTH_LOCKOUT_MS;
        printf("Access denied. Locked out for %d seconds.\r\n",
               AUTH_LOCKOUT_MS / 1000);
    } else {
        printf("Access denied. %d attempt(s) left.\r\n",
               AUTH_MAX_FAILURES - g_auth_failures);
    }
}

static void cmd_logout(void) {
    g_auth_active = false;
    printf("Authorized session ended.\r\n");
}

static void cmd_passwd(const char *old_pin, const char *new_pin) {
    size_t length;

    if (!require_auth()) {
        return;
    }
    if (old_pin == NULL || new_pin == NULL || strcmp(old_pin, g_admin_pin) != 0) {
        printf("Current PIN is incorrect.\r\n");
        return;
    }
    length = strlen(new_pin);
    if (length < 4 || length > 7) {
        printf("New PIN must be 4 to 7 characters.\r\n");
        return;
    }

    copy_str(g_admin_pin, sizeof(g_admin_pin), new_pin);
    if (nvs_save_pin(g_admin_pin) == ESP_OK) {
        printf("Admin PIN changed and saved.\r\n");
    } else {
        printf("Admin PIN changed in RAM (NVS save failed).\r\n");
    }
}

static void cmd_records_clear(void) {
    if (!require_auth()) {
        return;
    }
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_record_count = 0;
    g_record_head = 0;
    g_record_seq = 0;
    xSemaphoreGive(g_state_mutex);
    printf("In-RAM access records cleared (SD copy retained).\r\n");
}

static void console_dispatch(char *line) {
    char *argv[6];
    int argc = 0;

    for (char *token = strtok(line, " \t"); token != NULL && argc < 6;
         token = strtok(NULL, " \t")) {
        argv[argc++] = token;
    }
    if (argc == 0) {
        return;
    }

    if (eq_ci(argv[0], "help") || eq_ci(argv[0], "menu")) {
        print_help();
    } else if (eq_ci(argv[0], "status")) {
        cmd_status();
    } else if (eq_ci(argv[0], "occupancy")) {
        cmd_occupancy();
    } else if (eq_ci(argv[0], "rooms")) {
        cmd_rooms();
    } else if (eq_ci(argv[0], "users")) {
        cmd_users();
    } else if (eq_ci(argv[0], "records")) {
        if (argc >= 2 && eq_ci(argv[1], "clear")) {
            cmd_records_clear();
        } else {
            int limit = 10;
            if (argc >= 2) {
                parse_int(argv[1], &limit);
            }
            cmd_records(limit);
        }
    } else if (eq_ci(argv[0], "rfid")) {
        cmd_rfid();
    } else if (eq_ci(argv[0], "scan")) {
        int seconds = 5;
        if (argc >= 2) {
            parse_int(argv[1], &seconds);
        }
        cmd_scan(seconds);
    } else if (eq_ci(argv[0], "tap")) {
        if (argc < 3) {
            printf("Usage: tap <entry|exit> <UID>\r\n");
        } else {
            cmd_tap(argv[1], argv[2]);
        }
    } else if (eq_ci(argv[0], "time")) {
        if (argc >= 3 && eq_ci(argv[1], "set")) {
            cmd_time_set(argv[2]);
        } else {
            cmd_time_show();
        }
    } else if (eq_ci(argv[0], "schedule")) {
        if (argc >= 4 && eq_ci(argv[1], "set")) {
            cmd_schedule_set(argv[2], argv[3]);
        } else if (argc >= 3 && eq_ci(argv[1], "enable")) {
            cmd_schedule_toggle(true);
        } else if (argc >= 3 && eq_ci(argv[1], "disable")) {
            cmd_schedule_toggle(false);
        } else if (argc >= 4 && eq_ci(argv[1], "exit-outside")) {
            if (eq_ci(argv[2], "on")) {
                cmd_schedule_exit(true);
            } else if (eq_ci(argv[2], "off")) {
                cmd_schedule_exit(false);
            } else {
                printf("Usage: schedule exit-outside on|off\r\n");
            }
        } else {
            cmd_schedule_show();
        }
    } else if (eq_ci(argv[0], "auth")) {
        if (argc < 2) {
            printf("Usage: auth <PIN>\r\n");
        } else {
            cmd_auth(argv[1]);
        }
    } else if (eq_ci(argv[0], "logout")) {
        cmd_logout();
    } else if (eq_ci(argv[0], "passwd")) {
        if (argc < 3) {
            printf("Usage: passwd <old> <new>\r\n");
        } else {
            cmd_passwd(argv[1], argv[2]);
        }
    } else {
        printf("Unknown command '%s'. Type 'help'.\r\n", argv[0]);
    }
}

static void console_task(void *arg) {
    (void)arg;
    char line[128];
    size_t length = 0;
    bool last_was_cr = false;

    printf("cocis> ");
    fflush(stdout);

    while (1) {
        uint8_t ch = 0;
        int read = uart_read_bytes(UART_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            last_was_cr = (ch == '\r');
            printf("\r\n");
            line[length] = '\0';
            if (length > 0) {
                console_dispatch(line);
            }
            length = 0;
            printf("cocis> ");
            fflush(stdout);
            continue;
        }

        last_was_cr = false;
        if (ch == 0x08 || ch == 0x7F) {
            if (length > 0) {
                length--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (ch >= 32 && ch <= 126 && length + 1 < sizeof(line)) {
            line[length++] = (char)ch;
            printf("%c", ch);
            fflush(stdout);
        }
    }
}

/* ==========================================================================
 * STARTUP / HARDWARE INITIALISATION
 * ========================================================================== */

static esp_err_t configure_gpio(gpio_num_t pin, gpio_mode_t mode) {
    esp_err_t ret = gpio_reset_pin(pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d reset failed: %s", pin, esp_err_to_name(ret));
        return ret;
    }
    ret = gpio_set_direction(pin, mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d direction failed: %s", pin, esp_err_to_name(ret));
        return ret;
    }
    if (mode == GPIO_MODE_OUTPUT) {
        gpio_set_level(pin, 0);
    } else if (mode == GPIO_MODE_INPUT) {
        gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    }
    return ESP_OK;
}

static void run_led_self_test(void) {
    const gpio_num_t pins[] = { R1_LED_PIN, R2_LIGHT_PIN, GREEN_LED, RED_LED };
    for (int flash = 0; flash < 2; flash++) {
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            gpio_set_level(pins[i], 1);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
            gpio_set_level(pins[i], 0);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void servo_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_channel_config_t entry = {
        .gpio_num = ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE
    };
    ledc_channel_config_t exit = {
        .gpio_num = EXIT_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .intr_type = LEDC_INTR_DISABLE
    };

    if (ledc_timer_config(&timer) != ESP_OK) {
        ESP_LOGE(TAG, "Servo timer configuration failed");
        return;
    }
    if (ledc_channel_config(&entry) != ESP_OK ||
        ledc_channel_config(&exit) != ESP_OK) {
        ESP_LOGE(TAG, "Servo channel configuration failed");
        return;
    }
    set_servo(LEDC_CHANNEL_0, 1000);
    set_servo(LEDC_CHANNEL_1, 1000);
}

static void rfid_init_all(void) {
    int ready = 0;
    for (int i = 0; i < 2; i++) {
        if (mfrc522_init(&g_rfid[i])) {
            ready++;
        }
    }
    if (ready == 0) {
        ESP_LOGW(TAG, "No MFRC522 reader detected; serial 'tap' can be used");
    }
}

static void clock_task(void *arg) {
    (void)arg;
    int64_t last = now_ms();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t now = now_ms();
        int64_t elapsed = now - last;
        if (elapsed >= 1000) {
            uint32_t steps = (uint32_t)(elapsed / 1000);
            g_clock_s = (g_clock_s + steps) % 86400U;
            last += (int64_t)steps * 1000;
        }
    }
}

/* ==========================================================================
 * MAIN APPLICATION
 * ========================================================================== */

void app_main(void) {
    ESP_LOGI(TAG, "Booting CoCIS smart control and monitoring system...");

    /* ---- NVS: persisted schedule + admin PIN ---------------------------- */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition and retrying");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable (%s); defaults will be used",
                 esp_err_to_name(nvs_ret));
    }
    config_load();

    /* ---- Synchronisation primitives (requirement 2.7) ------------------- */
    g_state_mutex = xSemaphoreCreateMutex();
    g_sd_mutex = xSemaphoreCreateMutex();
    g_rfid_mutex = xSemaphoreCreateMutex();
    g_event_queue = xQueueCreate(EVENT_QUEUE_LEN, JSON_EVENT_SIZE);
    g_gate_queue = xQueueCreate(GATE_QUEUE_LEN, sizeof(gate_request_t));
    if (g_state_mutex == NULL || g_sd_mutex == NULL || g_rfid_mutex == NULL ||
        g_event_queue == NULL || g_gate_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create queues/mutexes; halting");
        return;
    }

    memset(&g_state, 0, sizeof(g_state));
    copy_str(g_state.last_terminal, sizeof(g_state.last_terminal), "SYSTEM");
    copy_str(g_state.last_event, sizeof(g_state.last_event), "BOOT");

    /* ---- GPIO outputs/inputs ------------------------------------------- */
    const gpio_num_t outputs[] = {
        R1_LED_PIN, R2_LIGHT_PIN, GREEN_LED, RED_LED, BUZZER
    };
    const gpio_num_t inputs[] = { R1_PIR_PIN, R2_PIR_PIN };
    bool gpio_ok = true;

    for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
        if (configure_gpio(outputs[i], GPIO_MODE_OUTPUT) != ESP_OK) {
            gpio_ok = false;
        }
    }
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        if (configure_gpio(inputs[i], GPIO_MODE_INPUT) != ESP_OK) {
            gpio_ok = false;
        }
    }
    if (gpio_ok) {
        run_led_self_test();
    } else {
        ESP_LOGE(TAG, "GPIO init failed; LED self-test skipped");
    }

    /* ---- SD card first: it initialises the shared SPI2 bus --------------- */
    g_sd_ready = (init_sd_card() == ESP_OK);
    if (g_sd_ready) {
        sd_prepare_files();
    } else {
        ESP_LOGW(TAG, "SD card unavailable; events stay in RAM/console");
    }

    /* ---- RFID readers join the same SPI2 bus ---------------------------- */
    rfid_init_all();

    /* ---- Gate servos ---------------------------------------------------- */
    servo_init();

    /* ---- Serial console ------------------------------------------------ */
    esp_err_t uart_ret = uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0);
    if (uart_ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(uart_ret));
    }

    /* ---- Terminal tasks ------------------------------------------------ */
    if (xTaskCreate(logger_task, "logger", 5120, NULL, 3, NULL) != pdPASS ||
        xTaskCreate(clock_task, "clock", 2048, NULL, 2, NULL) != pdPASS ||
        xTaskCreate(gate_action_task, "gate", 4096, NULL, 7, NULL) != pdPASS ||
        xTaskCreate(rfid_reader_task, "rfid", 4096, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(task_room1, "room1", 3072, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(task_room2, "room2", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create one or more terminal tasks");
    }

    printf("\r\n");
    printf("==============================================================\r\n");
    printf("   CoCIS SMART CONTROL AND MONITORING SYSTEM\r\n");
    printf("   ESP32 + Wokwi  |  GATE + LECTURE ROOM 1 + LECTURE ROOM 2\r\n");
    printf("==============================================================\r\n");
    printf("   SD card : %s\r\n", g_sd_ready ? "READY" : "NOT READY");
    printf("   RFID    : ENTRY %s | EXIT %s\r\n",
           g_rfid[0].ready ? "READY" : "NOT DETECTED",
           g_rfid[1].ready ? "READY" : "NOT DETECTED");
    printf("   Schedule: %02u:%02u-%02u:%02u (%s), exit outside %s\r\n",
           g_schedule.start_hour, g_schedule.start_min,
           g_schedule.end_hour, g_schedule.end_min,
           g_schedule.enabled ? "ENABLED" : "DISABLED",
           g_schedule.allow_exit_outside ? "ALLOWED" : "BLOCKED");
    printf("   Commands: type 'help'\r\n");
    if (strcmp(g_admin_pin, "2580") == 0) {
        printf("   Default admin PIN: 2580  (change with: passwd 2580 <new>)\r\n");
    } else {
        printf("   Admin PIN: custom value loaded from NVS\r\n");
    }
    printf("==============================================================\r\n");

    if (xTaskCreate(console_task, "console", 5120, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create console task");
    }
}

