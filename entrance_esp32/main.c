/*
 * CoCIS Smart Control - Entrance Terminal (Member 5)
 * Improved version with NVS persistence + better validation
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#define ADMIN_PASSWORD          "cocis2026"
#define DEFAULT_START_HOUR      7
#define DEFAULT_END_HOUR        19
#define NVS_NAMESPACE           "entrance"
#define NVS_KEY_START           "start_hour"
#define NVS_KEY_END             "end_hour"

#define LED_ACCESS_GRANTED      GPIO_NUM_2
#define LED_ACCESS_DENIED       GPIO_NUM_4
#define UART_NUM                UART_NUM_0
#define BUF_SIZE                128

static int admin_authenticated = 0;
static int schedule_start = DEFAULT_START_HOUR;
static int schedule_end   = DEFAULT_END_HOUR;
static int building_occupancy = 0;
static int simulated_hour = 10;          // Used for testing schedule

static const char *TAG = "ENTRANCE";

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
    nvs_commit(handle);
    nvs_close(handle);

    printf("✓ Schedule saved permanently.\n");
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
    printf("--------------------------------------------------\n");
    printf("1. View full system status\n");
    printf("2. View access schedule\n");
    printf("3. Admin login\n");
    printf("4. Admin logout\n");
    printf("5. Configure access schedule (Admin only)\n");
    printf("6. Change simulated current hour (for testing)\n");
    printf("7. View help\n");
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
    } else {
        printf("✗ Authentication failed. Access denied.\n");
    }
}

static void admin_logout(void)
{
    if (admin_authenticated) {
        admin_authenticated = 0;
        printf("Administrator logged out.\n");
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
    printf("✓ Access schedule updated to %02d:00 - %02d:00\n", schedule_start, schedule_end);
}

static void change_simulated_hour(void)
{
    int hour;
    if (!read_int("Enter simulated hour (0-23): ", &hour, 0, 23)) return;

    simulated_hour = hour;
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
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load previously saved schedule
    load_schedule_from_nvs();

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