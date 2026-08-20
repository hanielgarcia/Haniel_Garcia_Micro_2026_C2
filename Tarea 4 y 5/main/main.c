#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

#define WIFI_SSID      "TU_SSID"
#define WIFI_PASS      "TU_PASSWORD"
#define MQTT_BROKER    "mqtt://test.mosquitto.org"
#define MQTT_TOPIC     "esp32s3/test"
#define MQTT_TOPIC_CMD "esp32s3/cmd"
#define MQTT_TOPIC_STATE "esp32s3/state"

#define LED_GPIO       GPIO_NUM_2
#define BUTTON_GPIO    GPIO_NUM_0

static const char *TAG = "MQTT_EXAMPLE";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

typedef enum {
    STATE_OFF = 0,
    STATE_ON = 1
} led_state_t;

static led_state_t current_state = STATE_OFF;
static QueueHandle_t button_queue = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void IRAM_ATTR button_isr_handler(void* arg) {
    int gpio_num = (int)arg;
    xQueueSendFromISR(button_queue, &gpio_num, NULL);
}

static void led_set_state(led_state_t state) {
    current_state = state;
    gpio_set_level(LED_GPIO, state);
    ESP_LOGI(TAG, "LED: %s", state ? "ON" : "OFF");
    
    if (mqtt_client) {
        char msg[32];
        snprintf(msg, sizeof(msg), "{\"state\":%d}", state);
        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATE, msg, 0, 1, 0);
    }
}

static void state_machine_toggle(void) {
    led_state_t new_state = (current_state == STATE_OFF) ? STATE_ON : STATE_OFF;
    led_set_state(new_state);
}

static void button_task(void* arg) {
    int gpio_num;
    while (1) {
        if (xQueueReceive(button_queue, &gpio_num, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                ESP_LOGI(TAG, "Botón presionado - Cambiando estado");
                state_machine_toggle();
            }
        }
    }
}

static void gpio_init(void) {
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0);

    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);

    button_queue = xQueueCreate(10, sizeof(int));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, (void*)BUTTON_GPIO);
    xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
    
    ESP_LOGI(TAG, "GPIO inicializado - LED: GPIO%d, Botón: GPIO%d", LED_GPIO, BUTTON_GPIO);
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));
    
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
    
    ESP_LOGI(TAG, "WiFi inicializado, conectando a %s...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Conectado");
            esp_mqtt_client_subscribe(client, MQTT_TOPIC, 0);
            esp_mqtt_client_subscribe(client, MQTT_TOPIC_CMD, 0);
            esp_mqtt_client_publish(client, MQTT_TOPIC, "ESP32-S3 Online", 0, 1, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Desconectado");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "Suscrito a tópicos");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Mensaje recibido: Topic=%.*s, Data=%.*s", 
                     event->topic_len, event->topic, event->data_len, event->data);
            
            if (strncmp(event->topic, MQTT_TOPIC_CMD, event->topic_len) == 0) {
                if (strncmp(event->data, "ON", event->data_len) == 0) {
                    led_set_state(STATE_ON);
                } else if (strncmp(event->data, "OFF", event->data_len) == 0) {
                    led_set_state(STATE_OFF);
                } else if (strncmp(event->data, "TOGGLE", event->data_len) == 0) {
                    state_machine_toggle();
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            break;
        default:
            break;
    }
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 MQTT + State Machine ===");
    ESP_LOGI(TAG, "Chip: %s, Rev: %d, Núcleos: %d", 
             CONFIG_IDF_TARGET, CONFIG_ESP32S3_REV_MIN_0, CONFIG_FREERTOS_NUMBER_OF_CORES);
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    gpio_init();
    wifi_init();
    mqtt_app_start();
    
    int count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        count++;
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"heartbeat\":%d,\"state\":%d}", count, current_state);
        ESP_LOGI(TAG, "Publicando: %s", msg);
        if (mqtt_client) {
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, msg, 0, 1, 0);
        }
    }
}