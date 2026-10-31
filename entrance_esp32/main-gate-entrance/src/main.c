#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RFID_SCK GPIO_NUM_18
#define RFID_MISO GPIO_NUM_19
#define RFID_MOSI GPIO_NUM_23
#define ENTRY_CS GPIO_NUM_5
#define ENTRY_RST GPIO_NUM_22
#define EXIT_CS GPIO_NUM_17
#define EXIT_RST GPIO_NUM_21

#define ENTRY_SERVO GPIO_NUM_25
#define EXIT_SERVO GPIO_NUM_26
#define GREEN_LED GPIO_NUM_32
#define RED_LED GPIO_NUM_33
#define BUZZER GPIO_NUM_27

#define RFID_MAX_FRAME_SIZE 8
#define SERVO_PERIOD_US 20000
#define SERVO_CLOSED_US 500
#define SERVO_OPEN_US 1500

#define REG_COMMAND 0x01
#define REG_COM_I_EN 0x02
#define REG_COM_IRQ 0x04
#define REG_ERROR 0x06
#define REG_FIFO_DATA 0x09
#define REG_FIFO_LEVEL 0x0A
#define REG_BIT_FRAMING 0x0D
#define REG_TX_CONTROL 0x14
#define REG_TX_ASK 0x15
#define REG_MODE 0x11
#define REG_T_MODE 0x2A
#define REG_T_PRESCALER 0x2B
#define REG_T_RELOAD_H 0x2C
#define REG_T_RELOAD_L 0x2D

#define COMMAND_IDLE 0x00
#define COMMAND_TRANSCEIVE 0x0C
#define COMMAND_SOFT_RESET 0x0F
#define PICC_REQUEST_IDLE 0x26
#define PICC_ANTICOLLISION_CL1 0x93

static const char *TAG = "main_gate";

typedef struct {
    const char *uid;
    bool inside;
} registered_card_t;

static registered_card_t registered_cards[] = {
    { "DEADBEEF", false },
    { "11223344", false },
    { "A1B2C3D4", false },
};

static void rfid_write_reg(spi_device_handle_t device, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { (uint8_t)((reg << 1) & 0x7E), value };
    spi_transaction_t transaction = {
        .length = sizeof(tx) * 8,
        .tx_buffer = tx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(device, &transaction));
}

static uint8_t rfid_read_reg(spi_device_handle_t device, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(((reg << 1) & 0x7E) | 0x80), 0 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t transaction = {
        .length = sizeof(tx) * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(device, &transaction));
    return rx[1];
}

static void rfid_set_bits(spi_device_handle_t device, uint8_t reg, uint8_t mask)
{
    rfid_write_reg(device, reg, (uint8_t)(rfid_read_reg(device, reg) | mask));
}

static void rfid_clear_bits(spi_device_handle_t device, uint8_t reg, uint8_t mask)
{
    rfid_write_reg(device, reg, (uint8_t)(rfid_read_reg(device, reg) & ~mask));
}

static bool rfid_transceive(
    spi_device_handle_t device,
    const uint8_t *command,
    size_t command_size,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_size)
{
    rfid_write_reg(device, REG_COMMAND, COMMAND_IDLE);
    rfid_write_reg(device, REG_COM_IRQ, 0x7F);
    rfid_write_reg(device, REG_FIFO_LEVEL, 0x80);
    for (size_t i = 0; i < command_size; i++) {
        rfid_write_reg(device, REG_FIFO_DATA, command[i]);
    }

    rfid_write_reg(device, REG_COMMAND, COMMAND_TRANSCEIVE);
    rfid_set_bits(device, REG_BIT_FRAMING, 0x80);

    uint8_t irq = 0;
    for (int attempt = 0; attempt < 100; attempt++) {
        irq = rfid_read_reg(device, REG_COM_IRQ);
        if ((irq & 0x30) != 0) {
            break;
        }
        if ((irq & 0x01) != 0) {
            rfid_clear_bits(device, REG_BIT_FRAMING, 0x80);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    rfid_clear_bits(device, REG_BIT_FRAMING, 0x80);

    if ((irq & 0x30) == 0 || (rfid_read_reg(device, REG_ERROR) & 0x1B) != 0) {
        return false;
    }

    size_t bytes_available = rfid_read_reg(device, REG_FIFO_LEVEL);
    if (bytes_available == 0 || bytes_available > response_capacity) {
        return false;
    }
    for (size_t i = 0; i < bytes_available; i++) {
        response[i] = rfid_read_reg(device, REG_FIFO_DATA);
    }
    *response_size = bytes_available;
    return true;
}

static void rfid_initialize(spi_device_handle_t device, gpio_num_t reset_pin)
{
    gpio_set_level(reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    rfid_write_reg(device, REG_COMMAND, COMMAND_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    rfid_write_reg(device, REG_T_MODE, 0x8D);
    rfid_write_reg(device, REG_T_PRESCALER, 0x3E);
    rfid_write_reg(device, REG_T_RELOAD_H, 0x03);
    rfid_write_reg(device, REG_T_RELOAD_L, 0xE8);
    rfid_write_reg(device, REG_TX_ASK, 0x40);
    rfid_write_reg(device, REG_MODE, 0x3D);
    rfid_set_bits(device, REG_TX_CONTROL, 0x03);
}

static bool rfid_read_uid(spi_device_handle_t device, uint8_t uid[4])
{
    uint8_t command = PICC_REQUEST_IDLE;
    uint8_t response[RFID_MAX_FRAME_SIZE];
    size_t response_size = 0;

    rfid_write_reg(device, REG_BIT_FRAMING, 0x07);
    if (!rfid_transceive(device, &command, 1, response, sizeof(response), &response_size)
        || response_size != 2) {
        return false;
    }

    const uint8_t anticollision[] = { PICC_ANTICOLLISION_CL1, 0x20 };
    rfid_write_reg(device, REG_BIT_FRAMING, 0x00);
    if (!rfid_transceive(
            device, anticollision, sizeof(anticollision),
            response, sizeof(response), &response_size)
        || response_size != 5
        || (uint8_t)(response[0] ^ response[1] ^ response[2] ^ response[3]) != response[4]) {
        return false;
    }

    memcpy(uid, response, 4);
    return true;
}

static uint32_t servo_duty(uint32_t pulse_us)
{
    return (pulse_us * ((1U << 16) - 1U)) / SERVO_PERIOD_US;
}

static void set_servo(ledc_channel_t channel, uint32_t pulse_us)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, servo_duty(pulse_us)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void set_buzzer(bool enabled)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, enabled ? 512 : 0));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2));
}

static void beep(unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        set_buzzer(true);
        vTaskDelay(pdMS_TO_TICKS(100));
        set_buzzer(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void signal_access(bool granted, bool entry)
{
    gpio_set_level(GREEN_LED, granted);
    gpio_set_level(RED_LED, !granted);
    beep(granted ? 1 : 2);

    if (granted) {
        set_servo(entry ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1, SERVO_OPEN_US);
        vTaskDelay(pdMS_TO_TICKS(1500));
        set_servo(entry ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1, SERVO_CLOSED_US);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(GREEN_LED, 0);
    gpio_set_level(RED_LED, 0);
}

static int find_registered_card(const char *uid)
{
    for (size_t i = 0; i < sizeof(registered_cards) / sizeof(registered_cards[0]); i++) {
        if (strcmp(uid, registered_cards[i].uid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool card_is_approved(bool scanned, bool verified)
{
    return scanned && verified;
}

static void handle_card(const uint8_t uid_bytes[4], bool entry)
{
    char uid[9];
    snprintf(
        uid, sizeof(uid), "%02X%02X%02X%02X",
        uid_bytes[0], uid_bytes[1], uid_bytes[2], uid_bytes[3]);

    int card_index = find_registered_card(uid);
    bool verified = card_index >= 0 && (!entry || !registered_cards[card_index].inside);
    bool granted = card_is_approved(true, verified);
    if (granted) {
        registered_cards[card_index].inside = entry;
    }

    ESP_LOGI(
        TAG, "%s UID=%s scanned=YES verified=%s decision=%s",
        entry ? "ENTRY" : "EXIT", uid, verified ? "YES" : "NO",
        granted ? "YES" : "NO");
    signal_access(granted, entry);
}

// --- NEW: Handle string UIDs from the serial menu ---
static void handle_card_string(const char *uid, bool entry)
{
    int card_index = find_registered_card(uid);
    bool verified = card_index >= 0 && (!entry || !registered_cards[card_index].inside);
    bool granted = card_is_approved(true, verified);
    if (granted) {
        registered_cards[card_index].inside = entry;
    }

    ESP_LOGI(
        TAG, "%s UID=%s scanned=YES verified=%s decision=%s",
        entry ? "ENTRY" : "EXIT", uid, verified ? "YES" : "NO",
        granted ? "YES" : "NO");
    signal_access(granted, entry);
}

static void print_serial_menu(void)
{
    printf(
        "\nMAIN GATE CONTROL\n"
        "1. Simulate entry scan\n"
        "2. Simulate exit scan\n"
        "3. Simulate admin scan\n"
        "4. View current occupancy\n"
        "Select an option (1-4): ");
    fflush(stdout);
}

// --- NEW: Interactive Serial Menu Task ---
static void serial_test_task(void *arg)
{
    char input[128];

    print_serial_menu();
    while (true) {
        if (fgets(input, sizeof(input), stdin) == NULL) {
            ESP_LOGW(TAG, "Serial input closed; stopping the test menu.");
            vTaskDelete(NULL);
        }
        input[strcspn(input, "\r\n")] = '\0';

        char choice = input[0];
        if (choice == '1' || choice == '2') {
            printf("Enter Card UID: ");
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin) == NULL) {
                ESP_LOGW(TAG, "Serial input closed before a card UID was entered.");
                vTaskDelete(NULL);
            }
            input[strcspn(input, "\r\n")] = '\0';
            printf(
                "[SIMULATION] %s Scan: %s\n",
                choice == '1' ? "Entry" : "Exit", input);
            handle_card_string(input, choice == '1');
        } else if (choice == '3') {
            printf("[SIMULATION] Admin card scanned.\n");
            handle_card_string("11223344", true);
        } else if (choice == '4') {
            int count = 0;
            for (size_t i = 0; i < sizeof(registered_cards) / sizeof(registered_cards[0]); i++) {
                if (registered_cards[i].inside) {
                    count++;
                }
            }
            printf("People inside: %d\n", count);
        } else {
            printf("Invalid option. Enter 1, 2, 3, or 4.\n");
        }
        print_serial_menu();
    }
}

static spi_device_handle_t add_rfid_reader(gpio_num_t chip_select)
{
    spi_device_interface_config_t config = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = chip_select,
        .queue_size = 1,
    };
    spi_device_handle_t device;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &config, &device));
    return device;
}

static void configure_outputs(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << GREEN_LED) | (1ULL << RED_LED) | (1ULL << BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));

    gpio_config_t resets = {
        .pin_bit_mask = (1ULL << ENTRY_RST) | (1ULL << EXIT_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&resets));
    gpio_set_level(GREEN_LED, 0);
    gpio_set_level(RED_LED, 0);
    gpio_set_level(BUZZER, 0);
}

static void configure_pwm(void)
{
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&servo_timer));

    const ledc_channel_config_t servo_channels[] = {
        { .gpio_num = ENTRY_SERVO, .speed_mode = LEDC_LOW_SPEED_MODE,
          .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE,
          .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0 },
        { .gpio_num = EXIT_SERVO, .speed_mode = LEDC_LOW_SPEED_MODE,
          .channel = LEDC_CHANNEL_1, .intr_type = LEDC_INTR_DISABLE,
          .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0 },
    };
    for (size_t i = 0; i < sizeof(servo_channels) / sizeof(servo_channels[0]); i++) {
        ESP_ERROR_CHECK(ledc_channel_config(&servo_channels[i]));
    }

    ledc_timer_config_t buzzer_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&buzzer_timer));

    ledc_channel_config_t buzzer_channel = {
        .gpio_num = BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&buzzer_channel));
    set_servo(LEDC_CHANNEL_0, SERVO_CLOSED_US);
    set_servo(LEDC_CHANNEL_1, SERVO_CLOSED_US);
}

void app_main(void)
{
    configure_outputs();
    configure_pwm();

    // Start the menu task using the ESP-IDF console's standard input.
    xTaskCreate(serial_test_task, "serial_test_task", 4096, NULL, 5, NULL);

    spi_bus_config_t bus_config = {
        .mosi_io_num = RFID_MOSI,
        .miso_io_num = RFID_MISO,
        .sclk_io_num = RFID_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = RFID_MAX_FRAME_SIZE + 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));

    spi_device_handle_t entry_reader = add_rfid_reader(ENTRY_CS);
    spi_device_handle_t exit_reader = add_rfid_reader(EXIT_CS);
    rfid_initialize(entry_reader, ENTRY_RST);
    rfid_initialize(exit_reader, EXIT_RST);
    ESP_LOGI(TAG, "Gate controller ready; scan a registered four-byte UID.");

    bool entry_card_present = false;
    bool exit_card_present = false;
    while (true) {
        uint8_t uid[4];
        bool detected = rfid_read_uid(entry_reader, uid);
        if (detected && !entry_card_present) {
            handle_card(uid, true);
        }
        entry_card_present = detected;

        detected = rfid_read_uid(exit_reader, uid);
        if (detected && !exit_card_present) {
            handle_card(uid, false);
        }
        exit_card_present = detected;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
} 