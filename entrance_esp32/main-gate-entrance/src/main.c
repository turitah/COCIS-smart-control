#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "access_logic.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rc522_spi.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rc522.h"
#include "rc522_picc.h"

#define GREEN_LED GPIO_NUM_32
#define RED_LED GPIO_NUM_33
#define BUZZER GPIO_NUM_27

#define ENTRY_SERVO GPIO_NUM_25
#define EXIT_SERVO GPIO_NUM_26
#define ENTRY_BUTTON GPIO_NUM_16
#define EXIT_BUTTON GPIO_NUM_17

#define RFID_SPI_HOST SPI3_HOST
#define RFID_SCK GPIO_NUM_18
#define RFID_MISO GPIO_NUM_19
#define RFID_MOSI GPIO_NUM_23
#define ENTRY_RFID_CS GPIO_NUM_5
#define ENTRY_RFID_RST GPIO_NUM_21
#define EXIT_RFID_CS GPIO_NUM_14
#define EXIT_RFID_RST GPIO_NUM_22

#define SD_CS GPIO_NUM_13
#define SD_MOUNT_POINT "/sdcard"
#define SD_LOG_PATH SD_MOUNT_POINT "/access_log.csv"

#define SERVO_CHANNEL_ENTRY LEDC_CHANNEL_0
#define SERVO_CHANNEL_EXIT LEDC_CHANNEL_1
#define BUZZER_CHANNEL LEDC_CHANNEL_2
#define SERVO_TIMER LEDC_TIMER_0
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_DUTY_LOW 80

#define EVENT_LOG_CAPACITY 64
#define RFID_UID_TEXT_LEN (ACCESS_UID_MAX_LEN + 1)
#define ADMIN_SESSION_MS (120 * 1000)
#define ADMIN_PIN "2580"
#define ADMIN_CARD_UID "C0FFEE99"
#define DR_RITAH_UID "DEADBEEF"
#define JOHN_UID "11223344"
#define MARY_UID "55667788"
#define NFC_STAFF_UID "04112233445566"
#define UNKNOWN_UID "00000000"
#define TERMINAL_ID "MAIN_GATE"

#define DEFAULT_SCHEDULE_START_MINUTE (7 * 60)
#define DEFAULT_SCHEDULE_END_MINUTE (18 * 60)
#define DEFAULT_CLOCK_SECONDS (8 * 60 * 60)

typedef struct {
    const char *uid;
    const char *name;
    const char *role;
    bool admin;
} registered_user_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_ms;
    char clock[9];
    char direction[4];
    char uid[RFID_UID_TEXT_LEN];
    char name[32];
    char role[16];
    char source[20];
    char reason[32];
    char schedule[10];
    bool granted;
    bool admin;
    size_t occupancy;
} access_event_t;

typedef struct {
    ledc_channel_t channel;
    const char *label;
    int64_t close_at_ms;
} gate_state_t;

typedef struct {
    char uid[RFID_UID_TEXT_LEN];
    access_direction_t direction;
    char source[20];
} scan_request_t;

static const registered_user_t registered_users[] = {
    {DR_RITAH_UID, "Dr. Ritah", "LECTURER", false},
    {JOHN_UID, "John Student", "STUDENT", false},
    {MARY_UID, "Mary Student", "STUDENT", false},
    {NFC_STAFF_UID, "NFC Staff", "STAFF", false},
    {ADMIN_CARD_UID, "System Admin", "ADMIN", true},
};

static access_state_t access_state;
static access_event_t event_log[EVENT_LOG_CAPACITY];
static size_t event_count;
static size_t event_next;
static uint32_t next_event_sequence = 1;

static gate_state_t entry_gate = {.channel = SERVO_CHANNEL_ENTRY, .label = "ENTRY"};
static gate_state_t exit_gate = {.channel = SERVO_CHANNEL_EXIT, .label = "EXIT"};

static int64_t led_off_at_ms;
static int64_t buzzer_off_at_ms;
static int64_t admin_until_ms;

static bool schedule_enabled = true;
static int schedule_start_minute = DEFAULT_SCHEDULE_START_MINUTE;
static int schedule_end_minute = DEFAULT_SCHEDULE_END_MINUTE;

static int clock_base_seconds = DEFAULT_CLOCK_SECONDS;
static int64_t clock_base_ms;

static FILE *sd_log;
static bool sd_ready;

static QueueHandle_t scan_queue;

static rc522_driver_handle_t entry_driver;
static rc522_driver_handle_t exit_driver;
static rc522_handle_t entry_scanner;
static rc522_handle_t exit_scanner;

static void show_admin_menu(void);

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t servo_duty(uint32_t pulse_us)
{
    return (pulse_us * 65535U) / 20000U;
}

static void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0) return;
    if (source == NULL) source = "";
    snprintf(destination, destination_size, "%s", source);
}

static void trim_line(char *text)
{
    char *start = text;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
        *end = '\0';
    }
}

static int clock_now_seconds(void)
{
    const int64_t elapsed_seconds = (now_ms() - clock_base_ms) / 1000;
    int seconds = (int)((clock_base_seconds + elapsed_seconds) % (24 * 60 * 60));

    if (seconds < 0) {
        seconds += 24 * 60 * 60;
    }

    return seconds;
}

static void clock_now_text(char *buffer, size_t buffer_size)
{
    const int seconds = clock_now_seconds();

    snprintf(buffer, buffer_size, "%02d:%02d:%02d",
             seconds / 3600,
             (seconds / 60) % 60,
             seconds % 60);
}

static void clock_set_seconds(int seconds)
{
    clock_base_seconds = seconds;
    clock_base_ms = now_ms();
}

static bool admin_active(void)
{
    return admin_until_ms != 0 && now_ms() < admin_until_ms;
}

static const char *schedule_state_text(void)
{
    if (!schedule_enabled) return "DISABLED";

    return access_schedule_is_open(schedule_start_minute,
                                   schedule_end_minute,
                                   clock_now_seconds() / 60)
               ? "OPEN"
               : "CLOSED";
}

static bool schedule_is_open(void)
{
    return !schedule_enabled ||
           access_schedule_is_open(schedule_start_minute,
                                   schedule_end_minute,
                                   clock_now_seconds() / 60);
}

static char *format_schedule(char *buffer, size_t buffer_size)
{
    snprintf(buffer, buffer_size, "%02d:%02d-%02d:%02d%s",
             schedule_start_minute / 60,
             schedule_start_minute % 60,
             schedule_end_minute / 60,
             schedule_end_minute % 60,
             schedule_enabled ? "" : " OFF");
    return buffer;
}

static const registered_user_t *find_registered_user(const char *uid)
{
    size_t i;

    for (i = 0; i < sizeof(registered_users) / sizeof(registered_users[0]); ++i) {
        if (strcmp(registered_users[i].uid, uid) == 0) {
            return &registered_users[i];
        }
    }

    return NULL;
}

static bool user_is_inside(const char *uid)
{
    size_t i;

    for (i = 0; i < access_state.count; ++i) {
        if (strcmp(access_state.users[i].uid, uid) == 0) {
            return access_state.users[i].inside;
        }
    }

    return false;
}

static void set_servo(ledc_channel_t channel, uint32_t pulse_us)
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, servo_duty(pulse_us)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
}

static void set_buzzer(bool enabled, uint32_t frequency_hz)
{
    if (enabled) {
        ESP_ERROR_CHECK(ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency_hz));
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL,
                                      BUZZER_DUTY_LOW));
    } else {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0));
    }

    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL));
}

static void open_gate(gate_state_t *gate)
{
    set_servo(gate->channel, 2000);
    gate->close_at_ms = now_ms() + 2500;
    printf("%s GATE OPEN\n", gate->label);
}

static void close_gate(gate_state_t *gate)
{
    set_servo(gate->channel, 1000);
    gate->close_at_ms = 0;
    printf("%s GATE CLOSED\n", gate->label);
}

static void feedback_granted(void)
{
    gpio_set_level(RED_LED, 0);
    gpio_set_level(GREEN_LED, 1);
    set_buzzer(true, 2000);
    led_off_at_ms = now_ms() + 800;
    buzzer_off_at_ms = now_ms() + 350;
}

static void feedback_denied(void)
{
    gpio_set_level(GREEN_LED, 0);
    gpio_set_level(RED_LED, 1);
    set_buzzer(true, 700);
    led_off_at_ms = now_ms() + 1000;
    buzzer_off_at_ms = now_ms() + 700;
}

static void service_outputs(void)
{
    const int64_t now = now_ms();

    if (led_off_at_ms != 0 && now >= led_off_at_ms) {
        gpio_set_level(GREEN_LED, 0);
        gpio_set_level(RED_LED, 0);
        led_off_at_ms = 0;
    }

    if (buzzer_off_at_ms != 0 && now >= buzzer_off_at_ms) {
        set_buzzer(false, 0);
        buzzer_off_at_ms = 0;
    }

    if (entry_gate.close_at_ms != 0 && now >= entry_gate.close_at_ms) {
        close_gate(&entry_gate);
    }

    if (exit_gate.close_at_ms != 0 && now >= exit_gate.close_at_ms) {
        close_gate(&exit_gate);
    }
}

static const char *event_reason(access_result_t result,
                                access_direction_t direction,
                                bool schedule_open)
{
    if (result == ACCESS_GRANTED) {
        if (!schedule_enabled) return "SCHEDULE_DISABLED";
        if (direction == ACCESS_DIR_OUT && !schedule_open) return "AFTER_HOURS_EXIT";
        return "AUTHORIZED";
    }

    switch (result) {
    case ACCESS_DENIED_UNKNOWN_CARD: return "UNKNOWN_CARD";
    case ACCESS_DENIED_OUTSIDE_SCHEDULE: return "OUTSIDE_SCHEDULE";
    case ACCESS_DENIED_ALREADY_INSIDE: return "ALREADY_INSIDE";
    case ACCESS_DENIED_NOT_INSIDE: return "NOT_INSIDE";
    case ACCESS_DENIED_CAPACITY: return "CAPACITY";
    default: return "INVALID_REQUEST";
    }
}

static void write_csv_header(void)
{
    if (sd_log == NULL) return;

    fprintf(sd_log,
            "event_id,uptime_ms,clock,terminal,uid,name,role,direction,decision,"
            "reason,source,admin,schedule,occupancy\n");
    fflush(sd_log);
}

static void write_event_csv(const access_event_t *event)
{
    if (sd_log == NULL) return;

    fprintf(sd_log, "%lu,%lld,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%u\n",
            (unsigned long)event->sequence,
            (long long)event->timestamp_ms,
            event->clock,
            TERMINAL_ID,
            event->uid,
            event->name,
            event->role,
            event->direction,
            event->granted ? "GRANTED" : "DENIED",
            event->reason,
            event->source,
            event->admin ? "YES" : "NO",
            event->schedule,
            (unsigned)event->occupancy);

    if (fflush(sd_log) != 0 || ferror(sd_log)) {
        printf("[SD] Write failed; switching to RAM/serial logging only.\n");
        fclose(sd_log);
        sd_log = NULL;
        sd_ready = false;
    }
}

static void record_event(const char *direction,
                         const char *uid,
                         const char *name,
                         const char *role,
                         bool granted,
                         const char *reason,
                         const char *source,
                         bool admin,
                         const char *schedule)
{
    access_event_t *event = &event_log[event_next];

    memset(event, 0, sizeof(*event));
    event->sequence = next_event_sequence++;
    event->timestamp_ms = now_ms();
    clock_now_text(event->clock, sizeof(event->clock));
    copy_text(event->direction, sizeof(event->direction), direction);
    copy_text(event->uid, sizeof(event->uid), uid);
    copy_text(event->name, sizeof(event->name), name);
    copy_text(event->role, sizeof(event->role), role);
    copy_text(event->source, sizeof(event->source), source);
    copy_text(event->reason, sizeof(event->reason), reason);
    copy_text(event->schedule, sizeof(event->schedule), schedule);
    event->granted = granted;
    event->admin = admin;
    event->occupancy = access_state.occupancy;

    event_next = (event_next + 1) % EVENT_LOG_CAPACITY;
    if (event_count < EVENT_LOG_CAPACITY) {
        event_count++;
    }

    write_event_csv(event);

    printf("EVENT id=%lu terminal=%s uid=%s direction=%s decision=%s reason=%s "
           "name=%s source=%s admin=%s schedule=%s occupancy=%u clock=%s time_ms=%lld\n",
           (unsigned long)event->sequence,
           TERMINAL_ID,
           event->uid,
           event->direction,
           event->granted ? "GRANTED" : "DENIED",
           event->reason,
           event->name,
           event->source,
           event->admin ? "YES" : "NO",
           event->schedule,
           (unsigned)event->occupancy,
           event->clock,
           (long long)event->timestamp_ms);
}

static void record_admin_action(const char *reason, bool granted, const char *source)
{
    record_event("NA", "SYSTEM", "System Admin", "ADMIN", granted, reason,
                 source, true, schedule_state_text());
}

static bool enqueue_scan(const char *uid, access_direction_t direction, const char *source)
{
    scan_request_t request;

    if (scan_queue == NULL || uid == NULL) return false;

    memset(&request, 0, sizeof(request));
    copy_text(request.uid, sizeof(request.uid), uid);
    request.direction = direction;
    copy_text(request.source, sizeof(request.source), source);

    return xQueueSend(scan_queue, &request, 0) == pdTRUE;
}

static const char *direction_text(access_direction_t direction)
{
    return direction == ACCESS_DIR_IN ? "ENTRY" : "EXIT";
}

static void print_card_selection(const scan_request_t *request,
                                 const registered_user_t *user)
{
    printf("\n----------------------------------------\n");
    printf("              CARD SELECTED\n");
    printf("----------------------------------------\n");
    printf("Card      : %s\n", user != NULL ? user->name : "Unknown card");
    printf("UID       : %s\n", request->uid);
    printf("Role      : %s\n", user != NULL ? user->role : "NONE");
    printf("Direction : %s\n", direction_text(request->direction));
    printf("Source    : %s\n", request->source);
    printf("----------------------------------------\n");
}

static void print_verification(access_result_t result,
                               const registered_user_t *user,
                               const char *uid,
                               const char *reason,
                               access_direction_t direction)
{
    const bool granted = result == ACCESS_GRANTED;

    printf("[VERIFICATION] %s\n", granted ? "VERIFIED" : "NOT VERIFIED");
    printf("Decision  : %s\n", granted ? "GRANTED" : "DENIED");
    printf("Card      : %s\n", user != NULL ? user->name : "Unknown card");
    printf("UID       : %s\n", uid);
    printf("Direction : %s\n", direction_text(direction));
    printf("Reason    : %s\n", reason);
    printf("Occupancy : %u/%u\n",
           (unsigned)access_state.occupancy,
           (unsigned)access_state.capacity);
    printf("----------------------------------------\n");
}

static void process_scan(const scan_request_t *request)
{
    const registered_user_t *user = find_registered_user(request->uid);
    const bool schedule_open = schedule_is_open();
    const char *schedule = schedule_state_text();
    access_result_t result;
    const char *reason;

    print_card_selection(request, user);

    if (user != NULL && user->admin) {
        admin_until_ms = now_ms() + ADMIN_SESSION_MS;
        record_event(request->direction == ACCESS_DIR_IN ? "IN" : "OUT",
                     user->uid, user->name, user->role, true, "ADMIN_AUTH",
                     request->source, true, schedule);
        printf("[VERIFICATION] VERIFIED\n");
        printf("Decision  : GRANTED (admin session started)\n");
        printf("Card      : %s\n", user->name);
        printf("UID       : %s\n", request->uid);
        printf("----------------------------------------\n");
        printf("[ADMIN] Card authenticated. Session valid for 120 seconds.\n");
        feedback_granted();
        show_admin_menu();
        return;
    }

    result = access_process_card(&access_state, request->uid, request->direction,
                                 schedule_open);
    reason = event_reason(result, request->direction, schedule_open);

    record_event(request->direction == ACCESS_DIR_IN ? "IN" : "OUT",
                 request->uid,
                 user != NULL ? user->name : "Unknown",
                 user != NULL ? user->role : "NONE",
                 result == ACCESS_GRANTED,
                 reason,
                 request->source,
                 false,
                 schedule);

    print_verification(result, user, request->uid, reason, request->direction);

    if (result == ACCESS_GRANTED) {
        feedback_granted();
        open_gate(request->direction == ACCESS_DIR_IN ? &entry_gate : &exit_gate);
    } else {
        feedback_denied();
    }
}

static void normalize_uid(const char *raw, char *output, size_t output_size)
{
    size_t length = 0;

    if (output == NULL || output_size == 0) return;
    output[0] = '\0';

    if (raw == NULL) return;

    while (*raw != '\0') {
        const unsigned char character = (unsigned char)*raw++;

        if (isxdigit(character)) {
            if (length + 1 >= output_size) {
                output[0] = '\0';
                return;
            }

            output[length++] = (char)toupper(character);
        }
    }

    output[length] = '\0';
}

static void rc522_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    rc522_picc_state_changed_event_t *event = (rc522_picc_state_changed_event_t *)data;
    const access_direction_t direction = (access_direction_t)(uintptr_t)arg;
    char raw_uid[RC522_PICC_UID_STR_BUFFER_SIZE_MAX + 1];
    char uid[RFID_UID_TEXT_LEN];

    (void)base;

    if (event_id != RC522_EVENT_PICC_STATE_CHANGED || event == NULL ||
        event->picc == NULL || event->picc->state != RC522_PICC_STATE_ACTIVE) {
        return;
    }

    if (rc522_picc_uid_to_str(&event->picc->uid, raw_uid, sizeof(raw_uid)) != ESP_OK) {
        return;
    }

    normalize_uid(raw_uid, uid, sizeof(uid));
    if (uid[0] == '\0') return;

    enqueue_scan(uid, direction,
                 direction == ACCESS_DIR_IN ? "RFID_ENTRY" : "RFID_EXIT");
}

static void show_status(void)
{
    char clock[9];
    char schedule[32];
    size_t i;

    clock_now_text(clock, sizeof(clock));
    format_schedule(schedule, sizeof(schedule));

    printf("\n===================================\n");
    printf("          MAIN GATE STATUS\n");
    printf("===================================\n");
    printf("Clock: %s (software clock)\n", clock);
    printf("Schedule: %s (%s)\n", schedule, schedule_state_text());
    printf("Occupancy: %u/%u\n",
           (unsigned)access_state.occupancy,
           (unsigned)access_state.capacity);
    printf("Admin session: %s\n", admin_active() ? "ACTIVE" : "LOCKED");
    printf("SD logging: %s\n", sd_ready ? "READY" : "UNAVAILABLE");

    for (i = 0; i < sizeof(registered_users) / sizeof(registered_users[0]); ++i) {
        if (user_is_inside(registered_users[i].uid)) {
            printf("INSIDE: %s (%s) uid=%s\n",
                   registered_users[i].name,
                   registered_users[i].role,
                   registered_users[i].uid);
        }
    }

    printf("===================================\n");
}

static void show_event_log(void)
{
    size_t i;
    size_t start;

    printf("\n===================================\n");
    printf("          ACCESS EVENT LOG\n");
    printf("===================================\n");

    if (event_count == 0) {
        printf("No events recorded yet.\n");
        return;
    }

    start = event_count < EVENT_LOG_CAPACITY ? 0 : event_next;

    for (i = 0; i < event_count; ++i) {
        const access_event_t *event = &event_log[(start + i) % EVENT_LOG_CAPACITY];
        printf("#%lu %s %s uid=%s %s/%s %s occ=%u src=%s admin=%s\n",
               (unsigned long)event->sequence,
               event->clock,
               event->direction,
               event->uid,
               event->granted ? "GRANTED" : "DENIED",
               event->reason,
               event->name,
               (unsigned)event->occupancy,
               event->source,
               event->admin ? "YES" : "NO");
    }

    printf("===================================\n");
}

static void show_menu(void)
{
    printf("\n===================================\n");
    printf("       MAIN GATE CONTROL MENU\n");
    printf("===================================\n");
    printf("1. Simulate ENTRY scan: Dr. Ritah (%s)\n", DR_RITAH_UID);
    printf("2. Simulate EXIT scan:  Dr. Ritah (%s)\n", DR_RITAH_UID);
    printf("3. Simulate ENTRY scan: John (%s)\n", JOHN_UID);
    printf("4. Simulate EXIT scan:  John (%s)\n", JOHN_UID);
    printf("5. Simulate invalid card (%s)\n", UNKNOWN_UID);
    printf("6. Show gate status\n");
    printf("7. Show event log\n");
    printf("8. Admin login (PIN %s)\n", ADMIN_PIN);
    printf("9. Show this menu\n");
    printf("Select a command: ");
    fflush(stdout);
}

static void show_admin_menu(void)
{
    printf("\n===================================\n");
    printf("           ADMIN COMMANDS\n");
    printf("===================================\n");
    printf("a. Toggle schedule enforcement\n");
    printf("s. Set schedule start/end (HH:MM HH:MM)\n");
    printf("t. Set software clock (HH:MM or HH:MM:SS)\n");
    printf("c. Clear occupancy\n");
    printf("q. Log out\n");
    printf("Select a command: ");
    fflush(stdout);
}

static int read_line(char *buffer, size_t buffer_size, uint32_t timeout_ms)
{
    size_t length = 0;
    int64_t deadline;

    if (buffer == NULL || buffer_size == 0) return -1;
    buffer[0] = '\0';
    deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        uint8_t input;
        service_outputs();

        if (uart_read_bytes(UART_NUM_0, &input, 1, pdMS_TO_TICKS(50)) != 1) {
            continue;
        }

        if (input == '\r' || input == '\n') {
            if (length == 0) continue;
            printf("\n");
            fflush(stdout);
            return (int)length;
        }

        if (input == '\b' || input == 0x7f) {
            if (length > 0) {
                length--;
                buffer[length] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (isprint(input) && length + 1 < buffer_size) {
            buffer[length++] = (char)input;
            buffer[length] = '\0';
            printf("%c", input);
            fflush(stdout);
        }
    }

    return -1;
}

static bool save_config(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("gate_cfg", NVS_READWRITE, &handle);

    if (error != ESP_OK) {
        printf("[NVS] Unable to open configuration: %s\n", esp_err_to_name(error));
        return false;
    }

    error = nvs_set_u8(handle, "sched_en", schedule_enabled ? 1 : 0);
    if (error == ESP_OK) {
        error = nvs_set_i32(handle, "sched_start", schedule_start_minute);
    }
    if (error == ESP_OK) {
        error = nvs_set_i32(handle, "sched_end", schedule_end_minute);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }

    nvs_close(handle);

    if (error != ESP_OK) {
        printf("[NVS] Unable to save configuration: %s\n", esp_err_to_name(error));
        return false;
    }

    return true;
}

static void load_config(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("gate_cfg", NVS_READONLY, &handle);
    uint8_t enabled;
    int32_t start;
    int32_t end;

    if (error != ESP_OK) {
        return;
    }

    if (nvs_get_u8(handle, "sched_en", &enabled) == ESP_OK) {
        schedule_enabled = enabled != 0;
    }
    if (nvs_get_i32(handle, "sched_start", &start) == ESP_OK &&
        start >= 0 && start < 24 * 60) {
        schedule_start_minute = start;
    }
    if (nvs_get_i32(handle, "sched_end", &end) == ESP_OK &&
        end >= 0 && end < 24 * 60) {
        schedule_end_minute = end;
    }

    nvs_close(handle);
}

static void prompt_admin_login(void)
{
    char line[24];

    printf("\nEnter admin PIN: ");
    fflush(stdout);

    if (read_line(line, sizeof(line), 30000) < 0) {
        printf("\n[ADMIN] PIN entry timed out.\n");
        return;
    }

    trim_line(line);

    if (strcmp(line, ADMIN_PIN) == 0) {
        admin_until_ms = now_ms() + ADMIN_SESSION_MS;
        record_admin_action("ADMIN_LOGIN", true, "SERIAL_PIN");
        printf("\n[ADMIN] Login successful. Session valid for 120 seconds.\n");
        show_admin_menu();
        return;
    }

    record_event("NA", "SYSTEM", "Unknown", "NONE", false, "BAD_ADMIN_PIN",
                 "SERIAL_PIN", false, schedule_state_text());
    printf("\n[ADMIN] Login denied.\n");
}

static void toggle_schedule(void)
{
    schedule_enabled = !schedule_enabled;

    if (save_config()) {
        char schedule[32];
        format_schedule(schedule, sizeof(schedule));
        printf("\n[ADMIN] Schedule enforcement %s (%s).\n",
               schedule_enabled ? "ENABLED" : "DISABLED", schedule);
        record_admin_action(schedule_enabled ? "SCHEDULE_ENABLED" : "SCHEDULE_DISABLED",
                            true, "SERIAL");
    }
}

static void prompt_schedule(void)
{
    char line[64];
    int start_hour;
    int start_minute;
    int end_hour;
    int end_minute;
    int consumed = 0;

    printf("\nEnter schedule start and end (HH:MM HH:MM): ");
    fflush(stdout);

    if (read_line(line, sizeof(line), 30000) < 0) {
        printf("\n[ADMIN] Schedule entry timed out.\n");
        return;
    }

    if (!admin_active()) {
        printf("\n[ADMIN] Session expired; schedule change rejected.\n");
        record_event("NA", "SYSTEM", "Unknown", "NONE", false,
                     "ADMIN_SESSION_EXPIRED", "SERIAL", false,
                     schedule_state_text());
        return;
    }

    trim_line(line);

    if (sscanf(line, "%d:%d %d:%d%n",
               &start_hour, &start_minute, &end_hour, &end_minute,
               &consumed) != 4 || line[consumed] != '\0') {
        printf("\n[ADMIN] Invalid schedule format.\n");
        record_admin_action("SCHEDULE_INVALID", false, "SERIAL");
        return;
    }

    if (start_hour < 0 || start_hour > 23 || end_hour < 0 || end_hour > 23 ||
        start_minute < 0 || start_minute > 59 ||
        end_minute < 0 || end_minute > 59) {
        printf("\n[ADMIN] Invalid schedule time.\n");
        record_admin_action("SCHEDULE_INVALID", false, "SERIAL");
        return;
    }

    schedule_start_minute = start_hour * 60 + start_minute;
    schedule_end_minute = end_hour * 60 + end_minute;

    if (save_config()) {
        char schedule[32];
        format_schedule(schedule, sizeof(schedule));
        printf("\n[ADMIN] Schedule saved: %s\n", schedule);
        record_admin_action("SCHEDULE_SET", true, "SERIAL");
    }
}

static void prompt_clock(void)
{
    char line[32];
    int hour;
    int minute;
    int second = 0;
    int consumed = 0;
    int parsed;

    printf("\nEnter software clock time (HH:MM or HH:MM:SS): ");
    fflush(stdout);

    if (read_line(line, sizeof(line), 30000) < 0) {
        printf("\n[ADMIN] Clock entry timed out.\n");
        return;
    }

    if (!admin_active()) {
        printf("\n[ADMIN] Session expired; clock change rejected.\n");
        record_event("NA", "SYSTEM", "Unknown", "NONE", false,
                     "ADMIN_SESSION_EXPIRED", "SERIAL", false,
                     schedule_state_text());
        return;
    }

    trim_line(line);

    parsed = sscanf(line, "%d:%d:%d%n", &hour, &minute, &second, &consumed);
    if (parsed != 3 || line[consumed] != '\0') {
        consumed = 0;
        parsed = sscanf(line, "%d:%d%n", &hour, &minute, &consumed);
        second = 0;
    }

    if (parsed != 2 && parsed != 3) {
        printf("\n[ADMIN] Invalid clock format.\n");
        record_admin_action("CLOCK_INVALID", false, "SERIAL");
        return;
    }

    if (line[consumed] != '\0' ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        printf("\n[ADMIN] Invalid clock time.\n");
        record_admin_action("CLOCK_INVALID", false, "SERIAL");
        return;
    }

    clock_set_seconds(hour * 3600 + minute * 60 + second);
    printf("\n[ADMIN] Software clock set to %02d:%02d:%02d.\n", hour, minute, second);
    record_admin_action("CLOCK_SET", true, "SERIAL");
}

static void clear_occupancy(void)
{
    size_t i;

    for (i = 0; i < access_state.count; ++i) {
        access_state.users[i].inside = false;
    }
    access_state.occupancy = 0;

    printf("\n[ADMIN] Occupancy cleared.\n");
    record_admin_action("OCCUPANCY_CLEARED", true, "SERIAL");
}

static void handle_command(char raw_command)
{
    const char command = (char)tolower((unsigned char)raw_command);

    if (command == '\r' || command == '\n') return;

    if (admin_active()) {
        switch (command) {
        case 'a':
            toggle_schedule();
            return;
        case 's':
            prompt_schedule();
            return;
        case 't':
            prompt_clock();
            return;
        case 'c':
            clear_occupancy();
            return;
        case 'q':
            admin_until_ms = 0;
            printf("\n[ADMIN] Logged out.\n");
            show_menu();
            return;
        default:
            break;
        }
    }

    switch (command) {
    case '1':
        enqueue_scan(DR_RITAH_UID, ACCESS_DIR_IN, "SERIAL");
        break;
    case '2':
        enqueue_scan(DR_RITAH_UID, ACCESS_DIR_OUT, "SERIAL");
        break;
    case '3':
        enqueue_scan(JOHN_UID, ACCESS_DIR_IN, "SERIAL");
        break;
    case '4':
        enqueue_scan(JOHN_UID, ACCESS_DIR_OUT, "SERIAL");
        break;
    case '5':
        enqueue_scan(UNKNOWN_UID, ACCESS_DIR_IN, "SERIAL");
        break;
    case '6':
        show_status();
        break;
    case '7':
        show_event_log();
        break;
    case '8':
        if (admin_active()) {
            show_admin_menu();
        } else {
            prompt_admin_login();
        }
        break;
    case '9':
        show_menu();
        break;
    default:
        printf("\nUnknown command. Press 9 for help.\n");
        break;
    }
}

static void check_button(gpio_num_t pin,
                         bool *was_pressed,
                         access_direction_t direction,
                         const char *source)
{
    const bool pressed = gpio_get_level(pin) == 0;

    if (pressed && !*was_pressed) {
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level(pin) == 0) {
            *was_pressed = true;
            enqueue_scan(DR_RITAH_UID, direction, source);
        }
    } else if (!pressed) {
        *was_pressed = false;
    }
}

static void gate_task(void *argument)
{
    bool entry_button_pressed = false;
    bool exit_button_pressed = false;

    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(1200));
    uart_flush_input(UART_NUM_0);

    printf("\nMAIN GATE READY\n");
    show_menu();

    while (true) {
        scan_request_t request;
        uint8_t input;

        service_outputs();

        if (admin_until_ms != 0 && !admin_active()) {
            admin_until_ms = 0;
            printf("\n[ADMIN] Session expired.\n");
            show_menu();
        }

        check_button(ENTRY_BUTTON, &entry_button_pressed, ACCESS_DIR_IN, "BUTTON_ENTRY");
        check_button(EXIT_BUTTON, &exit_button_pressed, ACCESS_DIR_OUT, "BUTTON_EXIT");

        if (xQueueReceive(scan_queue, &request, pdMS_TO_TICKS(20)) == pdTRUE) {
            process_scan(&request);

            if (strcmp(request.source, "SERIAL") == 0) {
                printf("\nSelect a command (9 for menu): ");
                fflush(stdout);
            }
        }

        while (uart_read_bytes(UART_NUM_0, &input, 1, 0) == 1) {
            handle_command((char)input);
        }
    }
}

static void init_nvs(void)
{
    esp_err_t error = nvs_flash_init();

    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }

    ESP_ERROR_CHECK(error);
}

static void init_access_control(void)
{
    size_t i;

    access_state_init(&access_state, ACCESS_MAX_USERS);

    for (i = 0; i < sizeof(registered_users) / sizeof(registered_users[0]); ++i) {
        if (!access_register_user(&access_state, registered_users[i].uid)) {
            printf("[ACCESS] Failed to register %s\n", registered_users[i].uid);
        }
    }
}

static void init_gpio(void)
{
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << GREEN_LED) | (1ULL << RED_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << ENTRY_BUTTON) | (1ULL << EXIT_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_config));
    ESP_ERROR_CHECK(gpio_config(&button_config));
    gpio_set_level(GREEN_LED, 0);
    gpio_set_level(RED_LED, 0);
}

static void init_pwm(void)
{
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = SERVO_TIMER,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config_t buzzer_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = BUZZER_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t entry_servo = {
        .gpio_num = ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_CHANNEL_ENTRY,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
    };
    ledc_channel_config_t exit_servo = {
        .gpio_num = EXIT_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_CHANNEL_EXIT,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
    };
    ledc_channel_config_t buzzer_channel = {
        .gpio_num = BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&servo_timer));
    ESP_ERROR_CHECK(ledc_timer_config(&buzzer_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&entry_servo));
    ESP_ERROR_CHECK(ledc_channel_config(&exit_servo));
    ESP_ERROR_CHECK(ledc_channel_config(&buzzer_channel));

    set_servo(SERVO_CHANNEL_ENTRY, 1000);
    set_servo(SERVO_CHANNEL_EXIT, 1000);
    set_buzzer(false, 2000);
}

static void deassert_spi_devices(void)
{
    const gpio_config_t cs_config = {
        .pin_bit_mask = (1ULL << ENTRY_RFID_CS) | (1ULL << EXIT_RFID_CS) |
                        (1ULL << SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t rst_config = {
        .pin_bit_mask = (1ULL << ENTRY_RFID_RST) | (1ULL << EXIT_RFID_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cs_config));
    ESP_ERROR_CHECK(gpio_config(&rst_config));
    gpio_set_level(ENTRY_RFID_CS, 1);
    gpio_set_level(EXIT_RFID_CS, 1);
    gpio_set_level(SD_CS, 1);
    gpio_set_level(ENTRY_RFID_RST, 1);
    gpio_set_level(EXIT_RFID_RST, 1);
}

static void init_spi_bus(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = RFID_MOSI,
        .miso_io_num = RFID_MISO,
        .sclk_io_num = RFID_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(RFID_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    deassert_spi_devices();
}

static void init_sd(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t error;
    long size;

    host.slot = RFID_SPI_HOST;
    host.max_freq_khz = 400;
    slot_config.host_id = RFID_SPI_HOST;
    slot_config.gpio_cs = SD_CS;

    error = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                    &mount_config, &card);
    if (error != ESP_OK) {
        gpio_set_level(SD_CS, 1);
        printf("[SD] Mount failed: %s; RAM/serial logging only.\n",
               esp_err_to_name(error));
        return;
    }

    sd_log = fopen(SD_LOG_PATH, "a");
    if (sd_log == NULL) {
        printf("[SD] Mounted, but %s could not be opened.\n", SD_LOG_PATH);
        return;
    }

    fseek(sd_log, 0, SEEK_END);
    size = ftell(sd_log);
    if (size <= 0) {
        write_csv_header();
    }

    sd_ready = true;
    printf("[SD] Mounted at %s; access log: %s\n", SD_MOUNT_POINT, SD_LOG_PATH);
}

static void init_uart(void)
{
    esp_err_t error = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);

    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(error);
    }

    uart_flush_input(UART_NUM_0);
}

static void init_rfid(void)
{
    rc522_spi_config_t entry_config = {
        .host_id = RFID_SPI_HOST,
        .bus_config = NULL,
        .dev_config = {
            .spics_io_num = ENTRY_RFID_CS,
            .clock_speed_hz = 1000000,
        },
        .rst_io_num = ENTRY_RFID_RST,
    };
    rc522_spi_config_t exit_config = {
        .host_id = RFID_SPI_HOST,
        .bus_config = NULL,
        .dev_config = {
            .spics_io_num = EXIT_RFID_CS,
            .clock_speed_hz = 1000000,
        },
        .rst_io_num = EXIT_RFID_RST,
    };
    rc522_config_t entry_scanner_config = {
        .driver = entry_driver,
    };
    rc522_config_t exit_scanner_config = {
        .driver = exit_driver,
    };

    ESP_ERROR_CHECK(rc522_spi_create(&entry_config, &entry_driver));
    ESP_ERROR_CHECK(rc522_spi_create(&exit_config, &exit_driver));
    ESP_ERROR_CHECK(rc522_driver_install(entry_driver));
    ESP_ERROR_CHECK(rc522_driver_install(exit_driver));

    entry_scanner_config.driver = entry_driver;
    exit_scanner_config.driver = exit_driver;

    ESP_ERROR_CHECK(rc522_create(&entry_scanner_config, &entry_scanner));
    ESP_ERROR_CHECK(rc522_create(&exit_scanner_config, &exit_scanner));

    ESP_ERROR_CHECK(rc522_register_events(entry_scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                          rc522_event_handler,
                                          (void *)(uintptr_t)ACCESS_DIR_IN));
    ESP_ERROR_CHECK(rc522_register_events(exit_scanner, RC522_EVENT_PICC_STATE_CHANGED,
                                          rc522_event_handler,
                                          (void *)(uintptr_t)ACCESS_DIR_OUT));
}

static bool start_reader(rc522_handle_t scanner, const char *label)
{
    int attempt;

    for (attempt = 1; attempt <= 3; ++attempt) {
        esp_err_t error = rc522_start(scanner);

        if (error == ESP_OK) {
            printf("[RFID] %s reader ready.\n", label);
            return true;
        }

        printf("[RFID] %s reader start attempt %d failed: %s\n",
               label, attempt, esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    printf("[RFID] %s reader unavailable; use the serial menu or buttons.\n",
           label);
    return false;
}

static void start_rfid(void)
{
    const bool entry_ready = start_reader(entry_scanner, "ENTRY");
    const bool exit_ready = start_reader(exit_scanner, "EXIT");

    if (!entry_ready || !exit_ready) {
        printf("[RFID] One or more readers are offline; access logic remains usable.\n");
    }
}

static void run_startup_test(void)
{
    printf("Startup self-test: both gates open, green LED on, buzzer sounds.\n");
    set_servo(SERVO_CHANNEL_ENTRY, 2000);
    set_servo(SERVO_CHANNEL_EXIT, 2000);
    gpio_set_level(GREEN_LED, 1);
    set_buzzer(true, 2000);
    vTaskDelay(pdMS_TO_TICKS(700));
    set_buzzer(false, 2000);
    gpio_set_level(GREEN_LED, 0);
    set_servo(SERVO_CHANNEL_ENTRY, 1000);
    set_servo(SERVO_CHANNEL_EXIT, 1000);
    printf("Startup self-test complete.\n");
}

void app_main(void)
{
    esp_log_level_set("rc522", ESP_LOG_NONE);

    printf("\nMAIN GATE FIRMWARE STARTING\n");
    printf("Terminal: %s\n", TERMINAL_ID);

    clock_base_ms = now_ms();
    init_nvs();
    load_config();
    init_access_control();
    init_gpio();
    init_pwm();
    init_spi_bus();
    init_rfid();
    init_uart();

    scan_queue = xQueueCreate(8, sizeof(scan_request_t));
    if (scan_queue == NULL) {
        printf("FATAL: unable to create scan queue.\n");
        return;
    }

    run_startup_test();

    if (xTaskCreate(gate_task, "gate_control", 8192, NULL, 5, NULL) != pdPASS) {
        printf("FATAL: unable to create gate control task.\n");
        return;
    }

    start_rfid();
    init_sd();
}
