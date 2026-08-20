#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"

#define TAG "REACTION_TIMER"

#define GPIO_BUTTON_1     0
#define GPIO_BUTTON_2     1
#define GPIO_LED          2
#define GPIO_BUZZER       3

#define WIFI_SSID         "TU_SSID"
#define WIFI_PASS         "TU_PASSWORD"
#define MQTT_BROKER_URI   "mqtt://test.mosquitto.org"
#define MQTT_TOPIC        "reaction_timer/data"

typedef enum {
    STATE_WAIT_BUTTON1_PRESS,
    STATE_WAIT_RANDOM_DELAY,
    STATE_SIGNAL_ACTIVE,
    STATE_WAIT_BUTTON1_RELEASE,
    STATE_WAIT_BUTTON2_PRESS,
    STATE_SEND_DATA
} system_state_t;

static system_state_t current_state = STATE_WAIT_BUTTON1_PRESS;
static QueueHandle_t gpio_evt_queue = NULL;
static SemaphoreHandle_t state_mutex = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;

static int64_t button1_press_time = 0;
static int64_t signal_start_time = 0;
static int64_t button1_release_time = 0;
static int64_t button2_press_time = 0;
static int64_t random_delay_ms = 0;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT Published, msg_id=%d", event->msg_id);
            break;
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
        };
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
    }
}

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished.");
}

static void send_mqtt_data(int64_t reaction_time_ms, int64_t button1_hold_time_ms, int64_t random_delay_ms)
{
    if (mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT client not initialized");
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "reaction_time_ms", reaction_time_ms);
    cJSON_AddNumberToObject(root, "button1_hold_time_ms", button1_hold_time_ms);
    cJSON_AddNumberToObject(root, "random_delay_ms", random_delay_ms);
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000);

    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, json_str, 0, 1, 0);
        ESP_LOGI(TAG, "MQTT Publish sent, msg_id=%d: %s", msg_id, json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

static void gpio_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_BUTTON_1) | (1ULL << GPIO_BUTTON_2),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_LED) | (1ULL << GPIO_BUZZER);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_LED, 0);
    gpio_set_level(GPIO_BUZZER, 0);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON_1, gpio_isr_handler, (void*)GPIO_BUTTON_1);
    gpio_isr_handler_add(GPIO_BUTTON_2, gpio_isr_handler, (void*)GPIO_BUTTON_2);
}

static int64_t get_random_delay_ms(void)
{
    uint32_t rand_val = esp_random();
    return 2000 + (rand_val % 5001);
}

static void button_task(void* arg)
{
    uint32_t gpio_num;
    int button1_state = 1;
    int button2_state = 1;
    int last_button1_state = 1;
    int last_button2_state = 1;

    while (1) {
        if (xQueueReceive(gpio_evt_queue, &gpio_num, pdMS_TO_TICKS(10))) {
            int level = gpio_get_level(gpio_num);
            
            if (gpio_num == GPIO_BUTTON_1) {
                button1_state = level;
                if (last_button1_state == 1 && button1_state == 0) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    if (current_state == STATE_WAIT_BUTTON1_PRESS) {
                        button1_press_time = esp_timer_get_time();
                        random_delay_ms = get_random_delay_ms();
                        current_state = STATE_WAIT_RANDOM_DELAY;
                        ESP_LOGI(TAG, "Button 1 pressed, waiting %lld ms", random_delay_ms);
                    } else if (current_state == STATE_WAIT_BUTTON1_RELEASE) {
                        button1_release_time = esp_timer_get_time();
                        current_state = STATE_WAIT_BUTTON2_PRESS;
                        ESP_LOGI(TAG, "Button 1 released, waiting for Button 2");
                    }
                    xSemaphoreGive(state_mutex);
                }
                last_button1_state = button1_state;
            } else if (gpio_num == GPIO_BUTTON_2) {
                button2_state = level;
                if (last_button2_state == 1 && button2_state == 0) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    if (current_state == STATE_WAIT_BUTTON2_PRESS) {
                        button2_press_time = esp_timer_get_time();
                        current_state = STATE_SEND_DATA;
                        ESP_LOGI(TAG, "Button 2 pressed, calculating results");
                    }
                    xSemaphoreGive(state_mutex);
                }
                last_button2_state = button2_state;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void state_machine_task(void* arg)
{
    while (1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        system_state_t state = current_state;
        xSemaphoreGive(state_mutex);

        switch (state) {
            case STATE_WAIT_RANDOM_DELAY: {
                int64_t elapsed = (esp_timer_get_time() - button1_press_time) / 1000;
                if (elapsed >= random_delay_ms) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    current_state = STATE_SIGNAL_ACTIVE;
                    signal_start_time = esp_timer_get_time();
                    gpio_set_level(GPIO_LED, 1);
                    gpio_set_level(GPIO_BUZZER, 1);
                    ESP_LOGI(TAG, "Signal ON - LED and Buzzer activated");
                    xSemaphoreGive(state_mutex);
                }
                break;
            }
            case STATE_SIGNAL_ACTIVE: {
                int64_t elapsed = (esp_timer_get_time() - signal_start_time) / 1000;
                if (elapsed > 10000) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    current_state = STATE_WAIT_BUTTON1_PRESS;
                    gpio_set_level(GPIO_LED, 0);
                    gpio_set_level(GPIO_BUZZER, 0);
                    ESP_LOGI(TAG, "Timeout - resetting");
                    xSemaphoreGive(state_mutex);
                }
                break;
            }
            case STATE_SEND_DATA: {
                int64_t reaction_time_ms = (button2_press_time - signal_start_time) / 1000;
                int64_t button1_hold_time_ms = (button1_release_time - button1_press_time) / 1000;

                ESP_LOGI(TAG, "=== RESULTS ===");
                ESP_LOGI(TAG, "Reaction time: %lld ms", reaction_time_ms);
                ESP_LOGI(TAG, "Button 1 hold time: %lld ms", button1_hold_time_ms);
                ESP_LOGI(TAG, "Random delay: %lld ms", random_delay_ms);

                send_mqtt_data(reaction_time_ms, button1_hold_time_ms, random_delay_ms);

                gpio_set_level(GPIO_LED, 0);
                gpio_set_level(GPIO_BUZZER, 0);

                xSemaphoreTake(state_mutex, portMAX_DELAY);
                current_state = STATE_WAIT_BUTTON1_PRESS;
                xSemaphoreGive(state_mutex);
                break;
            }
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Reaction Timer System");
    ESP_LOGI(TAG, "ESP32-S3 - ESP-IDF v5.5.4");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    state_mutex = xSemaphoreCreateMutex();
    gpio_init();
    wifi_init_sta();

    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);
    xTaskCreate(state_machine_task, "state_machine_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "System ready. Press Button 1 and hold, wait for signal, release Button 1, press Button 2");
}