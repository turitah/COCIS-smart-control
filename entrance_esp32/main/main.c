/*
 * CoCIS entrance ESP-IDF scaffold.
 *
 * This is a serial-driven logic test harness, not yet the physical RFID reader
 * integration. It lets the team verify the access policy/state module while
 * the MFRC522 SPI driver and Wokwi wiring are added as the next milestone.
 *
 * Serial commands:
 *   in <UID>       simulate a card at the entry point
 *   out <UID>      simulate a card at the exit point
 *   status         show current occupancy
 *
 * Demo users: A1B2 and C3D4
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "access_logic.h"

static const char *TAG = "COCIS_ENTRANCE";
static access_state_t g_access;

static const char *result_text(access_result_t result)
{
    switch (result) {
        case ACCESS_GRANTED: return "GRANTED";
        case ACCESS_DENIED_UNKNOWN_CARD: return "DENIED: UNKNOWN CARD";
        case ACCESS_DENIED_OUTSIDE_SCHEDULE: return "DENIED: OUTSIDE ACCESS SCHEDULE";
        case ACCESS_DENIED_ALREADY_INSIDE: return "DENIED: USER ALREADY INSIDE";
        case ACCESS_DENIED_NOT_INSIDE: return "DENIED: USER NOT RECORDED INSIDE";
        case ACCESS_DENIED_CAPACITY: return "DENIED: BUILDING CAPACITY LIMIT";
        default: return "DENIED: INVALID REQUEST";
    }
}

void app_main(void)
{
    char line[64];
    char command[8];
    char uid[ACCESS_UID_MAX_LEN + 1];
    access_result_t result;

    access_state_init(&g_access, 100);
    (void)access_register_user(&g_access, "A1B2");
    (void)access_register_user(&g_access, "C3D4");

    ESP_LOGI(TAG, "Entrance logic test harness started.");
    ESP_LOGI(TAG, "Commands: in A1B2 | out A1B2 | status");
    ESP_LOGW(TAG, "RFID hardware driver is not integrated yet; commands simulate card reads.");

    while (1) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            command[0] = '\0';
            uid[0] = '\0';

            if (sscanf(line, "%7s %16s", command, uid) >= 1) {
                if (strcmp(command, "status") == 0) {
                    printf("BUILDING_OCCUPANCY=%u\n", (unsigned)g_access.occupancy);
                } else if (strcmp(command, "in") == 0 && uid[0] != '\0') {
                    result = access_process_card(&g_access, uid, ACCESS_DIR_IN, true);
                    printf("ENTRY UID=%s RESULT=%s OCCUPANCY=%u\n",
                           uid, result_text(result), (unsigned)g_access.occupancy);
                } else if (strcmp(command, "out") == 0 && uid[0] != '\0') {
                    result = access_process_card(&g_access, uid, ACCESS_DIR_OUT, true);
                    printf("EXIT UID=%s RESULT=%s OCCUPANCY=%u\n",
                           uid, result_text(result), (unsigned)g_access.occupancy);
                } else {
                    printf("Unknown command. Use: in <UID>, out <UID>, status\n");
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
