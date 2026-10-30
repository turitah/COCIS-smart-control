#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define LED_PIN GPIO_NUM_26
#define PIR_PIN GPIO_NUM_27
#define TIMEOUT_MS 10000

static int sequence = 0;

static void publish_status(int occupied, int light_on)
{
    sequence++;
    printf("{\"terminal_id\":\"ROOM1\",\"occupied\":%s,\"light_on\":%s,\"sequence\":%d}\n",
           occupied ? "true" : "false",
           light_on ? "true" : "false",
           sequence);
}

void app_main(void)
{
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIR_PIN, GPIO_MODE_INPUT);

    int light_on = 0;
    int64_t last_motion_ms = 0;

    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (gpio_get_level(PIR_PIN)) {
            last_motion_ms = now_ms;
            if (!light_on) {
                light_on = 1;
                gpio_set_level(LED_PIN, 1);
                publish_status(1, 1);
            }
        } else if (light_on && (now_ms - last_motion_ms) > TIMEOUT_MS) {
            light_on = 0;
            gpio_set_level(LED_PIN, 0);
            publish_status(0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}