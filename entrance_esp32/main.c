/*
 * CoCIS Smart Control - Entrance Terminal (Niwamanya)
 
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/semphr.h"

#define ADMIN_PASSWORD          "cocis2026"
#define DEFAULT_START_HOUR      7
#define DEFAULT_END_HOUR        19
#define NVS_NAMESPACE           "entrance"
#define NVS_KEY_START           "start_hour"
#define NVS_KEY_END             "end_hour"
#define SD_MOUNT_POINT          "/sdcard"
#define SD_LOG_PATH             SD_MOUNT_POINT "/events.csv"
#define SD_SCK_PIN              GPIO_NUM_18
#define SD_MISO_PIN             GPIO_NUM_19
#define SD_MOSI_PIN             GPIO_NUM_23
#define SD_CS_PIN               GPIO_NUM_5
#define RECENT_LOG_COUNT        10
#define LOG_LINE_SIZE           160

#define LED_ACCESS_GRANTED      GPIO_NUM_2
#define LED_ACCESS_DENIED       GPIO_NUM_4
#define UART_NUM                UART_NUM_0
#define BUF_SIZE                128

static int admin_authenticated = 0;
static int schedule_start = DEFAULT_START_HOUR;
static int schedule_end   = DEFAULT_END_HOUR;
static int building_occupancy = 0;
static int simulated_hour = 10;          //Just  for testing schedule
static bool sd_ready;
static sdmmc_card_t *sd_card;
static SemaphoreHandle_t sd_log_mutex;

static const char *TAG = "ENTRANCE";

// ---------------------------------------------------------------------
// SD CARD EVENT LOG
// ---------------------------------------------------------------------
static bool initialize_sd_card(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SD SPI bus: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SPI2_HOST;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &host, &slot_config, &mount_config, &sd_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount microSD at %s: %s",
                 SD_MOUNT_POINT, esp_err_to_name(err));
        return false;
    }

    sdmmc_card_print_info(stdout, sd_card);
    FILE *file = fopen(SD_LOG_PATH, "a+");
    if (file == NULL) {
        ESP_LOGE(TAG, "Card mounted, but event log %s cannot be opened", SD_LOG_PATH);
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = NULL;
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "Could not seek to the end of %s", SD_LOG_PATH);
        fclose(file);
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = NULL;
        return false;
    }
    if (ftell(file) == 0 &&
        fputs("uptime_ms,simulated_hour,event,occupancy,details\n", file) < 0) {
        ESP_LOGE(TAG, "Could not write the event-log header");
        fclose(file);
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = NULL;
        return false;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Could not close the event log after initialization");
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = NULL;
        return false;
    }

    sd_ready = true;
    ESP_LOGI(TAG, "SD event log ready: %s", SD_LOG_PATH);
    return true;
}

static void log_event(const char *event, const char *details)
{
    if (!sd_ready) {
        ESP_LOGW(TAG, "Event not stored; microSD is unavailable (%s)", event);
        return;
    }

    if (xSemaphoreTake(sd_log_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Could not acquire SD log mutex");
        return;
    }

    FILE *file = fopen(SD_LOG_PATH, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Could not open %s to store event %s", SD_LOG_PATH, event);
        xSemaphoreGive(sd_log_mutex);
        return;
    }

    TickType_t uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    int written = fprintf(file, "%lu,%d,%s,%d,%s\n",
                          (unsigned long)uptime_ms, simulated_hour, event,
                          building_occupancy, details);
    int close_result = fclose(file);
    if (written < 0 || close_result != 0) {
        ESP_LOGE(TAG, "Failed to write event %s to microSD", event);
    } else {
        ESP_LOGI(TAG, "Stored event: %s", event);
    }

    xSemaphoreGive(sd_log_mutex);
}

static void show_recent_events(void)
{
    if (!sd_ready) {
        printf("SD card unavailable; event log cannot be read.\n");
        return;
    }

    if (xSemaphoreTake(sd_log_mutex, portMAX_DELAY) != pdTRUE) {
        printf("Could not access the SD event log.\n");
        return;
    }

    FILE *file = fopen(SD_LOG_PATH, "r");
    if (file == NULL) {
        ESP_LOGE(TAG, "Could not open %s for reading", SD_LOG_PATH);
        xSemaphoreGive(sd_log_mutex);
        return;
    }

    char lines[RECENT_LOG_COUNT][LOG_LINE_SIZE] = {{0}};
    char line[LOG_LINE_SIZE];
    unsigned int line_count = 0;
    (void)fgets(line, sizeof(line), file); // Skip the CSV header.
    while (fgets(line, sizeof(line), file) != NULL) {
        snprintf(lines[line_count % RECENT_LOG_COUNT],
                 sizeof(lines[line_count % RECENT_LOG_COUNT]), "%s", line);
        line_count++;
    }
    bool read_error = ferror(file) != 0;
    fclose(file);
    xSemaphoreGive(sd_log_mutex);

    if (read_error) {
        printf("Error while reading the SD event log.\n");
        return;
    }

    printf("\n----- RECENT SD EVENTS -----\n");
    unsigned int first = line_count > RECENT_LOG_COUNT
                             ? line_count - RECENT_LOG_COUNT
                             : 0;
    for (unsigned int i = first; i < line_count; i++) {
        printf("%s", lines[i % RECENT_LOG_COUNT]);
    }
    if (line_count == 0) {
        printf("No events recorded yet.\n");
    }
    printf("----------------------------\n");
}

// ---------------------------------------------------------------------
// NVS HELPERS (save / load schedule)
// ---------------------------------------------------------------------
static void load_schedule_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved schedule found, using defaults");
        return;
    }

    int32_t start = DEFAULT_START_HOUR;
    int32_t end   = DEFAULT_END_HOUR;

    nvs_get_i32(handle, NVS_KEY_START, &start);
    nvs_get_i32(handle, NVS_KEY_END, &end);
    nvs_close(handle);

    schedule_start = (int)start;
    schedule_end   = (int)end;

    ESP_LOGI(TAG, "Loaded schedule from NVS: %02d:00 - %02d:00", schedule_start, schedule_end);
}

static void save_schedule_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing");
        return;
    }

    nvs_set_i32(handle, NVS_KEY_START, schedule_start);
    nvs_set_i32(handle, NVS_KEY_END, schedule_end);
    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        printf("✓ Schedule saved permanently.\n");
    } else {
        ESP_LOGE(TAG, "Failed to commit schedule: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------
// SERIAL INPUT HELPER
// ---------------------------------------------------------------------
static int read_line(char *buffer, int max_len)
{
    int i = 0;
    uint8_t ch;

    while (i < max_len - 1) {
        int len = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(300));
        if (len > 0) {
            if (ch == '\n' || ch == '\r') {
                if (i > 0) break;
            } else if (isprint(ch)) {
                buffer[i++] = ch;
                uart_write_bytes(UART_NUM, (const char *)&ch, 1); // echo
            }
        }
    }
    buffer[i] = '\0';
    printf("\n");
    return i;
}

// Read an integer safely. Returns 1 on success, 0 on failure.
static int read_int(const char *prompt, int *value, int min, int max)
{
    char buf[16];
    printf("%s", prompt);
    fflush(stdout);

    if (read_line(buf, sizeof(buf)) == 0) {
        printf("✗ No input received.\n");
        return 0;
    }

    // Check that the whole string is a number
    char *endptr;
    long val = strtol(buf, &endptr, 10);
    if (*endptr != '\0') {
        printf("✗ Invalid number. Please enter digits only.\n");
        return 0;
    }

    if (val < min || val > max) {
        printf("✗ Value must be between %d and %d.\n", min, max);
        return 0;
    }

    *value = (int)val;
    return 1;
}

// ---------------------------------------------------------------------
// SCHEDULE LOGIC
// ---------------------------------------------------------------------
static int is_within_schedule(void)
{
    if (schedule_start <= schedule_end) {
        return (simulated_hour >= schedule_start && simulated_hour < schedule_end);
    }
    // Handles overnight schedules (e.g. 22:00 - 06:00)
    return (simulated_hour >= schedule_start || simulated_hour < schedule_end);
}

// ---------------------------------------------------------------------
// MENU FUNCTIONS
// ---------------------------------------------------------------------
static void print_main_menu(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("   CoCIS Smart Control - Entrance Terminal\n");
    printf("==================================================\n");
    printf("Current Schedule   : %02d:00 - %02d:00\n", schedule_start, schedule_end);
    printf("Simulated Time     : %02d:00\n", simulated_hour);
    printf("Access Currently   : %s\n", is_within_schedule() ? "ALLOWED" : "DENIED");
    printf("Building Occupancy : %d people\n", building_occupancy);
    printf("Admin Status       : %s\n", admin_authenticated ? "AUTHENTICATED" : "Not logged in");
    printf("SD Event Log       : %s\n", sd_ready ? "READY" : "UNAVAILABLE");
    printf("--------------------------------------------------\n");
    printf("1. View full system status\n");
    printf("2. View access schedule\n");
    printf("3. Admin login\n");
    printf("4. Admin logout\n");
    printf("5. Configure access schedule (Admin only)\n");
    printf("6. Change simulated current hour (for testing)\n");
    printf("7. View help\n");
    printf("8. View recent SD card events\n");
    printf("0. Refresh menu\n");
    printf("--------------------------------------------------\n");
    printf("Enter choice: ");
    fflush(stdout);
}

static void show_status(void)
{
    printf("\n----- SYSTEM STATUS -----\n");
    printf("Access Schedule     : %02d:00 - %02d:00\n", schedule_start, schedule_end);
    printf("Simulated Time      : %02d:00\n", simulated_hour);
    printf("Access Currently    : %s\n", is_within_schedule() ? "ALLOWED" : "DENIED");
    printf("Building Occupancy  : %d\n", building_occupancy);
    printf("SD Event Log       : %s\n", sd_ready ? "READY" : "UNAVAILABLE");
    printf("Admin Logged In     : %s\n", admin_authenticated ? "Yes" : "No");
    printf("-------------------------\n");
}

static void admin_login(void)
{
    char password[32];

    if (admin_authenticated) {
        printf("Already logged in as administrator.\n");
        return;
    }

    printf("Enter admin password: ");
    fflush(stdout);
    read_line(password, sizeof(password));

    if (strcmp(password, ADMIN_PASSWORD) == 0) {
        admin_authenticated = 1;
        printf("✓ Administrator authentication successful.\n");
        log_event("admin_login", "success");
    } else {
        printf("✗ Authentication failed. Access denied.\n");
        log_event("admin_login", "failed");
    }
}

static void admin_logout(void)
{
    if (admin_authenticated) {
        admin_authenticated = 0;
        printf("Administrator logged out.\n");
        log_event("admin_logout", "serial_menu");
    } else {
        printf("No administrator is currently logged in.\n");
    }
}

static void configure_schedule(void)
{
    int start, end;

    if (!admin_authenticated) {
        printf("✗ Access denied. Administrator login required.\n");
        return;
    }

    printf("Current schedule: %02d:00 - %02d:00\n", schedule_start, schedule_end);

    if (!read_int("Enter new start hour (0-23): ", &start, 0, 23)) return;
    if (!read_int("Enter new end hour (0-23): ", &end, 0, 23)) return;

    schedule_start = start;
    schedule_end   = end;

    save_schedule_to_nvs();
    char details[32];
    snprintf(details, sizeof(details), "%02d:00-%02d:00", start, end);
    log_event("schedule_changed", details);
    printf("✓ Access schedule updated to %02d:00 - %02d:00\n", schedule_start, schedule_end);
}

static void change_simulated_hour(void)
{
    int hour;
    if (!read_int("Enter simulated hour (0-23): ", &hour, 0, 23)) return;

    simulated_hour = hour;
    char details[16];
    snprintf(details, sizeof(details), "%02d:00", hour);
    log_event("test_hour_changed", details);
    printf("✓ Simulated time set to %02d:00\n", simulated_hour);
    printf("Access is now: %s\n", is_within_schedule() ? "ALLOWED" : "DENIED");
}

static void show_help(void)
{
    printf("\n----- HELP -----\n");
    printf("This is the Entrance Terminal management interface.\n");
    printf("Only authenticated administrators can change the access schedule.\n");
    printf("Default admin password : %s\n", ADMIN_PASSWORD);
    printf("Schedule is saved permanently (survives reboot).\n");
    printf("Use option 6 to change the simulated time and test the schedule.\n");
    printf("Outside the allowed hours, entry and exit should be denied.\n");
    printf("----------------\n");
}

// ---------------------------------------------------------------------
// MENU TASK
// ---------------------------------------------------------------------
static void menu_task(void *arg)
{
    char line[16];

    vTaskDelay(pdMS_TO_TICKS(1500));

    while (1) {
        print_main_menu();

        int len = read_line(line, sizeof(line));
        if (len == 0) continue;

        int choice = atoi(line);

        switch (choice) {
            case 1: show_status(); break;
            case 2:
                printf("Access is allowed from %02d:00 to %02d:00\n",
                       schedule_start, schedule_end);
                break;
            case 3: admin_login(); break;
            case 4: admin_logout(); break;
            case 5: configure_schedule(); break;
            case 6: change_simulated_hour(); break;
            case 7: show_help(); break;
            case 8: show_recent_events(); break;
            case 0: break;
            default:
                printf("Invalid choice. Try again.\n");
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// ---------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------
void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load previously saved schedule
    load_schedule_from_nvs();
    sd_log_mutex = xSemaphoreCreateMutex();
    if (sd_log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SD event-log mutex");
    } else {
        initialize_sd_card();
    }
    log_event("system_start", "entrance_started");

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);

    // LEDs
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_ACCESS_GRANTED) | (1ULL << LED_ACCESS_DENIED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LED_ACCESS_GRANTED, 0);
    gpio_set_level(LED_ACCESS_DENIED, 0);

    ESP_LOGI(TAG, "Entrance terminal starting...");
    ESP_LOGI(TAG, "Default admin password: %s", ADMIN_PASSWORD);

    xTaskCreate(menu_task, "menu_task", 4096, NULL, 5, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}