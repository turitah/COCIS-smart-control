#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "access_logic.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GREEN_LED GPIO_NUM_32
#define RED_LED GPIO_NUM_33
#define BUZZER GPIO_NUM_27

#define ENTRY_SERVO GPIO_NUM_25
#define EXIT_SERVO GPIO_NUM_26

#define RFID_SCK GPIO_NUM_18
#define RFID_MISO GPIO_NUM_19
#define RFID_MOSI GPIO_NUM_23
#define ENTRY_RFID_CS GPIO_NUM_5
#define ENTRY_RFID_RST GPIO_NUM_22
#define EXIT_RFID_CS GPIO_NUM_17
#define EXIT_RFID_RST GPIO_NUM_21

#define EVENT_LOG_CAPACITY 32
#define RFID_UID_TEXT_LEN (ACCESS_UID_MAX_LEN + 1)

typedef struct {
    const char *uid;
    const char *name;
    const char *role;
} registered_user_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_ms;
    access_direction_t direction;
    access_result_t result;
    char uid[RFID_UID_TEXT_LEN];
} access_event_t;

typedef struct {
    ledc_channel_t channel;
    int64_t close_at_ms;
} gate_state_t;

static const registered_user_t registered_users[] = {
    {"DEADBEEF", "Dr. Ritah", "LECTURER"},
    {"11223344", "John Student", "STUDENT"},
    {"A1B2C3D4", "System Admin", "ADMIN"}
};

static access_state_t access_state;
static access_event_t event_log[EVENT_LOG_CAPACITY];
static size_t event_count;
static size_t event_next;
static uint32_t next_event_sequence = 1;
static gate_state_t entry_gate = {.channel = LEDC_CHANNEL_0};
static gate_state_t exit_gate = {.channel = LEDC_CHANNEL_1};

// --- FORWARD DECLARATIONS ---
static const char *result_name(access_result_t result);
static const char *direction_name(access_direction_t direction);
static void record_event(const char *uid, access_direction_t direction, access_result_t result);
static void process_card(const char *uid, access_direction_t direction);
static void show_status(void);
static void show_menu(void);
static void service_outputs(void);

// --- HARDWARE FUNCTIONS ---
static uint32_t servo_duty(uint32_t pulse_us) {
    return (pulse_us * 65535U) / 20000U;
}

static void set_servo(ledc_channel_t channel, uint32_t pulse_us) {
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, servo_duty(pulse_us)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void set_gate(gate_state_t *gate) {
    set_servo(gate->channel, 2000); // Open
    gate->close_at_ms = esp_timer_get_time() / 1000 + 2000; // Close in 2s
}

static void service_outputs(void) {
    const int64_t now = esp_timer_get_time() / 1000;

    if (entry_gate.close_at_ms != 0 && now >= entry_gate.close_at_ms) {
        set_servo(entry_gate.channel, 1000);
        entry_gate.close_at_ms = 0;
        printf("\nENTRY GATE CLOSED\n");
    }
    if (exit_gate.close_at_ms != 0 && now >= exit_gate.close_at_ms) {
        set_servo(exit_gate.channel, 1000);
        exit_gate.close_at_ms = 0;
        printf("\nEXIT GATE CLOSED\n");
    }
}

// --- FIXED BLINK FUNCTION (Controls LED and Buzzer together) ---
static void blink_led_and_beep(gpio_num_t pin) {
    for (int i = 0; i < 2; i++) {
        gpio_set_level(pin, 1);
        gpio_set_level(BUZZER, 1); // Buzzer ON
        vTaskDelay(pdMS_TO_TICKS(250)); 
        
        gpio_set_level(pin, 0);
        gpio_set_level(BUZZER, 0); // Buzzer OFF
        vTaskDelay(pdMS_TO_TICKS(250)); 
    }
}

// --- LOGIC FUNCTIONS ---
static const char *result_name(access_result_t result) {
    switch (result) {
    case ACCESS_GRANTED: return "GRANTED";
    case ACCESS_DENIED_UNKNOWN_CARD: return "UNKNOWN_CARD";
    case ACCESS_DENIED_OUTSIDE_SCHEDULE: return "OUTSIDE_SCHEDULE";
    case ACCESS_DENIED_ALREADY_INSIDE: return "ALREADY_INSIDE";
    case ACCESS_DENIED_NOT_INSIDE: return "NOT_INSIDE";
    case ACCESS_DENIED_CAPACITY: return "CAPACITY";
    }
    return "INVALID_REQUEST";
}

static const char *direction_name(access_direction_t direction) {
    return direction == ACCESS_DIR_IN ? "IN" : "OUT";
}

static void record_event(const char *uid, access_direction_t direction, access_result_t result) {
    access_event_t *event = &event_log[event_next];
    const size_t last_uid_index = RFID_UID_TEXT_LEN - 1;

    event->sequence = next_event_sequence++;
    event->timestamp_ms = esp_timer_get_time() / 1000;
    event->direction = direction;
    event->result = result;
    (void)strncpy(event->uid, uid, last_uid_index);
    event->uid[last_uid_index] = '\0';
    event_next = (event_next + 1) % EVENT_LOG_CAPACITY;
    if (event_count < EVENT_LOG_CAPACITY) event_count++;

    printf("EVENT id=%lu terminal=MAIN_GATE uid=%s direction=%s decision=%s time_ms=%lld\n",
           (unsigned long)event->sequence, event->uid,
           direction_name(direction), result_name(result),
           (long long)event->timestamp_ms);
}

static bool verify_card(const char *uid, access_direction_t direction, access_result_t *result_out) {
    const access_result_t result = access_process_card(&access_state, uid, direction, true);
    *result_out = result;

    if (result == ACCESS_GRANTED) {
        printf("\n[VERIFIED: YES] Card is valid.\n");
        return true;
    } else {
        printf("\n[VERIFIED: NO] Access denied. Reason: %s\n", result_name(result));
        return false;
    }
}

static void process_card(const char *uid, access_direction_t direction) {
    access_result_t result;
    bool granted = verify_card(uid, direction, &result);

    record_event(uid, direction, result);

    if (granted) {
        blink_led_and_beep(GREEN_LED); // Blink Green + Beep
        printf("%s GRANTED; occupancy=%u\n", direction_name(direction), (unsigned)access_state.occupancy);
        set_gate(direction == ACCESS_DIR_IN ? &entry_gate : &exit_gate);
        printf("%s GATE OPEN\n", direction_name(direction));
    } else {
        blink_led_and_beep(RED_LED); // Blink Red + Buzz
        printf("ACCESS DENIED: %s uid=%s\n", result_name(result), uid);
    }
}

static void show_status(void) {
    printf("\n[OCCUPANCY: %u]\n", (unsigned)access_state.occupancy);
    for (size_t i = 0; i < sizeof(registered_users) / sizeof(registered_users[0]); ++i) {
        if (access_state.users[i].inside) {
            printf("INSIDE: %s (%s)\n", registered_users[i].name, registered_users[i].role);
        }
    }
}

// --- MENU SYSTEM ---
static void show_menu(void) {
    printf("\n===================================\n");
    printf("       MAIN GATE CONTROL MENU      \n");
    printf("===================================\n");
    printf("1. Simulate ENTRY Scan (Valid Card)\n");
    printf("2. Simulate EXIT Scan (Valid Card)\n");
    printf("3. Simulate Admin Scan\n");
    printf("4. Simulate INVALID Card Scan\n");
    printf("5. View Current Occupancy\n");
    printf("Select an option (1-5): ");
    fflush(stdout);
}

static void serial_task(void *arg) {
    uint8_t input;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    uart_flush_input(UART_NUM_0);    
    
    printf("\nMAIN GATE READY.\n");
    show_menu();

    while (true) {
        service_outputs(); 

        if (uart_read_bytes(UART_NUM_0, &input, 1, pdMS_TO_TICKS(50)) == 1) {
            
            if (input == '1') {
                printf("\n\n[SIMULATION] Valid Card scanned at ENTRY.\n");
                process_card("DEADBEEF", ACCESS_DIR_IN);
                show_menu();
            } 
            else if (input == '2') {
                printf("\n\n[SIMULATION] Valid Card scanned at EXIT.\n");
                process_card("DEADBEEF", ACCESS_DIR_OUT);
                show_menu();
            } 
            else if (input == '3') {
                printf("\n\n[SIMULATION] Admin Card scanned.\n");
                process_card("A1B2C3D4", ACCESS_DIR_IN);
                show_menu();
            } 
            else if (input == '4') {
                printf("\n\n[SIMULATION] Invalid Card scanned.\n");
                process_card("00000000", ACCESS_DIR_IN);
                show_menu();
            } 
            else if (input == '5') {
                show_status();
                show_menu();
            }
            else if (input == '\r' || input == '\n') {
                // Ignore Enter key
            }
            else {
                printf("\nInvalid option. Please press 1, 2, 3, 4, or 5.\n");
                show_menu();
            }
        }
    }
}

void app_main(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << GREEN_LED) | (1ULL << RED_LED) | (1ULL << BUZZER),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_channel_config_t entry_servo = {
        .gpio_num = ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0
    };
    ledc_channel_config_t exit_servo = {
        .gpio_num = EXIT_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0
    };

    access_state_init(&access_state, ACCESS_MAX_USERS);
    for (size_t i = 0; i < sizeof(registered_users) / sizeof(registered_users[0]); ++i) {
        ESP_ERROR_CHECK(access_register_user(&access_state, registered_users[i].uid) ? ESP_OK : ESP_FAIL);
    }

    ESP_ERROR_CHECK(gpio_config(&output_config));
    ESP_ERROR_CHECK(ledc_timer_config(&servo_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&entry_servo));
    ESP_ERROR_CHECK(ledc_channel_config(&exit_servo));
    set_servo(LEDC_CHANNEL_0, 1000);
    set_servo(LEDC_CHANNEL_1, 1000);

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_flush_input(UART_NUM_0));
    
    xTaskCreate(serial_task, "gate_control", 6144, NULL, 5, NULL);
} 