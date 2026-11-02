#include "room_monitor.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define PIR_PIN             GPIO_NUM_13
#define LIGHT_PIN           GPIO_NUM_26
#define EMPTY_AFTER_MS      10000
#define WIFI_SSID           "Wokwi-GUEST"
#define MQTT_URI            "mqtt://broker.hivemq.com:1883"

static const char *TAG = "ROOM_MONITOR";
static int current_room;
static volatile bool occupied;
static volatile bool mqtt_connected;
static volatile bool wifi_has_ip;
static esp_mqtt_client_handle_t mqtt_client;

static void publish_state(void)
{
    if (mqtt_client == NULL || !mqtt_connected) {
        return;
    }

    char topic[32];
    char payload[80];
    snprintf(topic, sizeof(topic), "room%d/occupancy", current_room);
    snprintf(payload, sizeof(payload),
             "{\"room\":%d,\"occupied\":%s,\"light\":%s}",
             current_room,
             occupied ? "true" : "false",
             occupied ? "true" : "false");

    int message_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    if (message_id < 0) {
        ESP_LOGE(TAG, "Failed to publish occupancy for room %d", current_room);
    } else {
        ESP_LOGI(TAG, "Published %s to %s", payload, topic);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected; the client will reconnect");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT transport error");
        break;
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_has_ip = false;
        mqtt_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_has_ip = true;
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void start_wifi(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() == NULL ? ESP_FAIL : ESP_OK);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, WIFI_SSID, sizeof(WIFI_SSID));
    wifi_config.sta.channel = 6;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void start_mqtt_when_online(void *arg)
{
    (void)arg;
    while (!wifi_has_ip) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    char client_id[24];
    snprintf(client_id, sizeof(client_id), "cocis-room%d", current_room);
    esp_mqtt_client_config_t config = {
        .broker.address.uri = MQTT_URI,
        .credentials.client_id = client_id,
    };
    mqtt_client = esp_mqtt_client_init(&config);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
    vTaskDelete(NULL);
}

void room_monitor_start(int room_id)
{
    current_room = room_id;
    occupied = false;
    mqtt_connected = false;
    wifi_has_ip = false;

    gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << PIR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));

    gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << LIGHT_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));
    ESP_ERROR_CHECK(gpio_set_level(LIGHT_PIN, 0));

    ESP_LOGI(TAG, "Room %d ready; PIR GPIO %d, light GPIO %d",
             current_room, PIR_PIN, LIGHT_PIN);
    start_wifi();

    BaseType_t task_result = xTaskCreate(
        start_mqtt_when_online, "mqtt_start", 4096, NULL, 4, NULL);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT startup task");
    }

    TickType_t last_motion = 0;
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if (gpio_get_level(PIR_PIN) == 1) {
            last_motion = now;
            if (!occupied) {
                occupied = true;
                ESP_ERROR_CHECK(gpio_set_level(LIGHT_PIN, 1));
                ESP_LOGI(TAG, "Room %d occupied; light ON", current_room);
                publish_state();
            }
        } else if (occupied &&
                   (now - last_motion) > pdMS_TO_TICKS(EMPTY_AFTER_MS)) {
            occupied = false;
            ESP_ERROR_CHECK(gpio_set_level(LIGHT_PIN, 0));
            ESP_LOGI(TAG, "Room %d empty; light OFF", current_room);
            publish_state();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
