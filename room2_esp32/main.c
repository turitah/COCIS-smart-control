#define ROOM_ID     2
#define ENABLE_NET  1

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#if ENABLE_NET
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#endif

// ---------------------------------------------------------------------
// 1. PINS AND SETTINGS  (change here only)
// ---------------------------------------------------------------------
#define PIR_PIN      GPIO_NUM_13   // PIR sensor output
#define LIGHT_PIN    GPIO_NUM_26   // room light (LED)
#define EMPTY_AFTER_MS  10000      // no motion for this long = room empty

#define STR_(x) #x
#define STR(x)  STR_(x)
#define TOPIC      "SE-22/room" STR(ROOM_ID) "/occupancy"
#define CLIENT_ID  "SE-22-room" STR(ROOM_ID)       // must be unique on the broker
#define MQTT_URI   "mqtt://broker.hivemq.com:1883"

// ---------------------------------------------------------------------
// 2. STATE
// ---------------------------------------------------------------------
static int occupied = 0;   // 0 = empty, 1 = occupied

#if ENABLE_NET
static esp_mqtt_client_handle_t mqtt = NULL;
static volatile int mqtt_up = 0;
static volatile int got_ip  = 0;
#endif

// ---------------------------------------------------------------------
// 3. PUBLISH THE ROOM STATE (does nothing when ENABLE_NET is 0)
// ---------------------------------------------------------------------
static void publish_state(void)
{
#if ENABLE_NET
    if (mqtt && mqtt_up) {
        const char *json = occupied ? "{\"occupied\":true}" : "{\"occupied\":false}";
        // retain = 1: the Entrance gets the latest state even if it connects later
        esp_mqtt_client_publish(mqtt, TOPIC, json, 0, 1, 1);
        printf("Published %s to %s\n", json, TOPIC);
    }
#endif
}

// ---------------------------------------------------------------------
// 4. NETWORK (WiFi + MQTT)
// ---------------------------------------------------------------------
#if ENABLE_NET
static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_up = 1;
        printf("MQTT connected\n");
        publish_state();            // tell the Entrance the current state
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_up = 0;
        break;
    default:
        break;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        got_ip = 0;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        got_ip = 1;
        printf("WiFi connected\n");
    }
}

static void wifi_start(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL);

    wifi_config_t wc = {0};
    strcpy((char *)wc.sta.ssid, "Wokwi-GUEST");   // open network, no password
    wc.sta.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();
}

static void net_task(void *arg)
{
    while (!got_ip) vTaskDelay(pdMS_TO_TICKS(500));

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = CLIENT_ID
    };
    mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_mqtt_client_start(mqtt);
    vTaskDelete(NULL);
}
#endif

// ---------------------------------------------------------------------
// 5. MAIN: read the PIR, switch the light, publish changes
// ---------------------------------------------------------------------
void app_main(void)
{
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&in);

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << LIGHT_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&out);
    gpio_set_level(LIGHT_PIN, 0);

    printf("Room %d terminal ready. Waiting for motion...\n", ROOM_ID);

#if ENABLE_NET
    wifi_start();
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
#endif

    TickType_t last_motion = 0;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        if (gpio_get_level(PIR_PIN) == 1) {
            last_motion = now;
            if (!occupied) {                       // empty -> occupied
                occupied = 1;
                gpio_set_level(LIGHT_PIN, 1);
                printf("Room %d: OCCUPIED, light ON\n", ROOM_ID);
                publish_state();
            }
        } else if (occupied &&
                   (now - last_motion) > pdMS_TO_TICKS(EMPTY_AFTER_MS)) {
            occupied = 0;                          // occupied -> empty
            gpio_set_level(LIGHT_PIN, 0);
            printf("Room %d: EMPTY, light OFF\n", ROOM_ID);
            publish_state();
        }

        vTaskDelay(pdMS_TO_TICKS(100));            // check 10 times a second
    }
}