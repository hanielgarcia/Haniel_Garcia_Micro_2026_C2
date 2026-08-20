/*
 * TAREA 7 - Sistema de Medicion de Reaccion Humana - ESP32-S3
 * -------------------------------------------------------------
 * Flujo correcto:
 *   1. Usuario MANTIENE presionado PB1 (HOLD)
 *   2. Micro espera delay aleatorio 2-6s
 *   3. Enciende LED + BUZZER = SENAL
 *   4. Usuario SUELTA PB1  -> se mide tiempo de liberacion (ms)
 *   5. Usuario PRESIONA PB2 -> se mide tiempo total de reaccion (ms)
 *   6. Se envia todo por MQTT en JSON con precision de 1 ms
 *
 * Correcciones vs TAREA6:
 *   - Maquina de estados fija (antes nunca salia de SIGNAL_ACTIVE)
 *   - Deteccion de RELEASE por flanco 0->1 (antes detectaba press de nuevo)
 *   - Antitrampa: si suelta PB1 antes de la senal = FALSE START
 *   - Debounce 50ms + timestamp de evento en ISR con esp_timer_get_time()
 *   - Medicion con esp_timer_get_time() [resolucion 1 us, reportado 1 ms]
 *   - MQTT publica 4 metricas + estado (ok/false_start/timeout)
 *
 * Autor: Haniel Garcia - Micro 2026 C2
 * ESP-IDF v5.5.4 - ESP32-S3 - FreeRTOS 1000 Hz
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"

#define TAG "REACTION_T7"

// ==================== CONFIGURACION HARDWARE ====================
// Si usas DevKit con boton BOOT en GPIO0, cambia a GPIO4 para evitar conflicto
#define GPIO_BUTTON_HOLD   4      // PB1: mantener presionado y luego soltar (antes GPIO0)
#define GPIO_BUTTON_REACT  5      // PB2: presionar tras la senal    (antes GPIO1)
#define GPIO_LED           2      // LED senal
#define GPIO_BUZZER        3      // Buzzer activo

// Alternativa si quieres usar botones del DevKit (descomenta):
// #define GPIO_BUTTON_HOLD   0
// #define GPIO_BUTTON_REACT  1

// ==================== CONFIGURACION WIFI / MQTT ====================
#define WIFI_SSID         "TU_SSID"
#define WIFI_PASS         "TU_PASSWORD"
#define MQTT_BROKER_URI   "mqtt://test.mosquitto.org"
#define MQTT_TOPIC_DATA   "reaction_timer/data"
#define MQTT_TOPIC_STATUS "reaction_timer/status"

// ==================== PARAMETROS ====================
#define RANDOM_DELAY_MIN_MS   2000   // 2 s
#define RANDOM_DELAY_MAX_MS   6000   // 6 s
#define SIGNAL_TIMEOUT_MS    10000   // si no suelta PB1 en 10s -> reset
#define BUTTON2_TIMEOUT_MS    5000   // si no presiona PB2 en 5s -> reset
#define DEBOUNCE_MS             50

// ==================== MAQUINA DE ESTADOS ====================
typedef enum {
    STATE_IDLE = 0,              // esperando que presione PB1
    STATE_ARMED,                 // PB1 presionado, esperando delay aleatorio
    STATE_SIGNAL_ON,             // senal encendida, esperando que SUELTE PB1
    STATE_WAIT_BTN2,             // PB1 soltado, esperando que PRESIONE PB2
    STATE_FALSE_START,           // solto antes de tiempo
} system_state_t;

static volatile system_state_t current_state = STATE_IDLE;
static QueueHandle_t gpio_evt_queue = NULL;
static SemaphoreHandle_t state_mutex = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Timestamps en microsegundos (esp_timer_get_time)
static int64_t t_hold_press_us   = 0;
static int64_t t_signal_on_us    = 0;
static int64_t t_hold_release_us = 0;
static int64_t t_react_press_us  = 0;
static int64_t random_delay_ms   = 0;

// debounce por GPIO
static int64_t last_isr_time_us[40] = {0};

typedef struct {
    uint32_t gpio;
    int64_t  isr_time_us;
    int      level;
} gpio_event_t;

// ==================== ISR ====================
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    int64_t now = esp_timer_get_time();
    // debounce en ISR (50ms)
    if (now - last_isr_time_us[gpio_num] < DEBOUNCE_MS * 1000) return;
    last_isr_time_us[gpio_num] = now;

    gpio_event_t evt = {
        .gpio = gpio_num,
        .isr_time_us = now,
        .level = gpio_get_level(gpio_num)
    };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(gpio_evt_queue, &evt, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// ==================== MQTT ====================
static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Conectado");
            mqtt_connected = true;
            esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_STATUS, "{\"status\":\"online\",\"device\":\"esp32s3_t7\"}", 0, 1, 1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Desconectado");
            mqtt_connected = false;
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT Publicado msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            break;
        default: break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi desconectado, reconectando...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "WiFi OK, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        esp_mqtt_client_config_t mqtt_cfg = {
            .broker.address.uri = MQTT_BROKER_URI,
        };
        if (mqtt_client) {
            esp_mqtt_client_destroy(mqtt_client);
        }
        mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
        esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(mqtt_client);
    }
}

static void wifi_init_sta(void) {
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
    ESP_LOGI(TAG, "WiFi init OK, conectando a %s ...", WIFI_SSID);
}

static void mqtt_publish_json(cJSON* root) {
    if (!mqtt_connected || mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT no conectado, dato no enviado");
        cJSON_Delete(root);
        return;
    }
    char* json = cJSON_PrintUnformatted(root);
    if (json) {
        int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC_DATA, json, 0, 1, 0);
        ESP_LOGI(TAG, "MQTT publish [%d]: %s", msg_id, json);
        free(json);
    }
    cJSON_Delete(root);
}

static void publish_result_ok(void) {
    int64_t reaction_release_ms = (t_hold_release_us - t_signal_on_us) / 1000;
    int64_t reaction_total_ms   = (t_react_press_us - t_signal_on_us) / 1000;
    int64_t travel_ms           = (t_react_press_us - t_hold_release_us) / 1000;
    int64_t hold_ms             = (t_hold_release_us - t_hold_press_us) / 1000;

    // clamp negativos (si hay jitter)
    if (reaction_release_ms < 0) reaction_release_ms = 0;
    if (reaction_total_ms < 0)   reaction_total_ms = 0;
    if (travel_ms < 0)           travel_ms = 0;

    ESP_LOGI(TAG, "========== RESULTADO OK ==========");
    ESP_LOGI(TAG, " Random delay : %lld ms", random_delay_ms);
    ESP_LOGI(TAG, " Hold PB1     : %lld ms", hold_ms);
    ESP_LOGI(TAG, " Reaccion suelta PB1 (senal->suelta) : %lld ms", reaction_release_ms);
    ESP_LOGI(TAG, " Reaccion total  (senal->PB2)        : %lld ms", reaction_total_ms);
    ESP_LOGI(TAG, " Traslado (suelta->PB2)              : %lld ms", travel_ms);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "random_delay_ms", random_delay_ms);
    cJSON_AddNumberToObject(root, "hold_time_ms", hold_ms);
    cJSON_AddNumberToObject(root, "release_time_ms", reaction_release_ms);
    cJSON_AddNumberToObject(root, "reaction_time_ms", reaction_total_ms);
    cJSON_AddNumberToObject(root, "travel_time_ms", travel_ms);
    // compatibilidad con TAREA6
    cJSON_AddNumberToObject(root, "button1_hold_time_ms", hold_ms);
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "timestamp_us", esp_timer_get_time());
    mqtt_publish_json(root);
}

static void publish_false_start(void) {
    int64_t hold_ms = (esp_timer_get_time() - t_hold_press_us) / 1000;
    ESP_LOGW(TAG, "!!! FALSE START - solto PB1 antes de la senal (hold %lld ms, delay era %lld ms)", hold_ms, random_delay_ms);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "false_start");
    cJSON_AddNumberToObject(root, "random_delay_ms", random_delay_ms);
    cJSON_AddNumberToObject(root, "hold_time_ms", hold_ms);
    cJSON_AddNumberToObject(root, "reaction_time_ms", 0);
    cJSON_AddNumberToObject(root, "release_time_ms", 0);
    cJSON_AddNumberToObject(root, "travel_time_ms", 0);
    cJSON_AddNumberToObject(root, "button1_hold_time_ms", hold_ms);
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000);
    mqtt_publish_json(root);
}

static void publish_timeout(const char* stage) {
    ESP_LOGW(TAG, "TIMEOUT en %s", stage);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "timeout");
    cJSON_AddStringToObject(root, "stage", stage);
    cJSON_AddNumberToObject(root, "random_delay_ms", random_delay_ms);
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000);
    mqtt_publish_json(root);
}

// ==================== GPIO ====================
static void gpio_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_BUTTON_HOLD) | (1ULL << GPIO_BUTTON_REACT),
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

    gpio_evt_queue = xQueueCreate(20, sizeof(gpio_event_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_BUTTON_HOLD,  gpio_isr_handler, (void*)GPIO_BUTTON_HOLD);
    gpio_isr_handler_add(GPIO_BUTTON_REACT, gpio_isr_handler, (void*)GPIO_BUTTON_REACT);
    ESP_LOGI(TAG, "GPIO OK: HOLD=%d REACT=%d LED=%d BUZZER=%d", GPIO_BUTTON_HOLD, GPIO_BUTTON_REACT, GPIO_LED, GPIO_BUZZER);
}

static int64_t get_random_delay_ms(void) {
    uint32_t r = esp_random();
    return RANDOM_DELAY_MIN_MS + (r % (RANDOM_DELAY_MAX_MS - RANDOM_DELAY_MIN_MS + 1));
}

// Helper: blink LED para feedback
static void blink_error(int times) {
    for (int i=0;i<times;i++) {
        gpio_set_level(GPIO_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(GPIO_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

// ==================== TAREAS ====================
static void button_task(void* arg) {
    gpio_event_t evt;
    while (1) {
        if (xQueueReceive(gpio_evt_queue, &evt, pdMS_TO_TICKS(10))) {
            // evt.level: 0=presionado (pullup), 1=soltado
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            system_state_t st = current_state;
            xSemaphoreGive(state_mutex);

            ESP_LOGD(TAG, "GPIO %lu level %d state %d t=%lld us", evt.gpio, evt.level, st, evt.isr_time_us);

            if (evt.gpio == GPIO_BUTTON_HOLD) {
                if (evt.level == 0) { // PRESIONADO
                    if (st == STATE_IDLE) {
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        t_hold_press_us = evt.isr_time_us; // usar tiempo de ISR para precision
                        random_delay_ms = get_random_delay_ms();
                        current_state = STATE_ARMED;
                        xSemaphoreGive(state_mutex);
                        ESP_LOGI(TAG, "[IDLE->ARMED] PB1 presionado, delay aleatorio %lld ms (2-6s)", random_delay_ms);
                        // LED tenue indicando armado
                        gpio_set_level(GPIO_LED, 0);
                    } else {
                        ESP_LOGD(TAG, "PB1 press ignorado en estado %d", st);
                    }
                } else { // SOLTADO (1)
                    if (st == STATE_ARMED) {
                        // FALSE START
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        current_state = STATE_FALSE_START;
                        xSemaphoreGive(state_mutex);
                        publish_false_start();
                        blink_error(3);
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        current_state = STATE_IDLE;
                        xSemaphoreGive(state_mutex);
                        ESP_LOGI(TAG, "[FALSE START->IDLE] Listo para nuevo intento. Mantenga PB1 presionado y espere la senal.");
                    } else if (st == STATE_SIGNAL_ON) {
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        t_hold_release_us = evt.isr_time_us;
                        current_state = STATE_WAIT_BTN2;
                        xSemaphoreGive(state_mutex);
                        int64_t rel_ms = (t_hold_release_us - t_signal_on_us)/1000;
                        ESP_LOGI(TAG, "[SIGNAL->WAIT_BTN2] PB1 soltado! Reaccion soltada: %lld ms. Ahora presione PB2!", rel_ms);
                        // mantener LED encendido hasta completar
                    } else {
                        ESP_LOGD(TAG, "PB1 release ignorado en estado %d", st);
                    }
                }
            } else if (evt.gpio == GPIO_BUTTON_REACT) {
                if (evt.level == 0) { // PB2 PRESIONADO
                    if (st == STATE_WAIT_BTN2) {
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        t_react_press_us = evt.isr_time_us;
                        // calcular y publicar inmediatamente
                        xSemaphoreGive(state_mutex);
                        publish_result_ok();
                        // apagar senal
                        gpio_set_level(GPIO_LED, 0);
                        gpio_set_level(GPIO_BUZZER, 0);
                        // feedback: blink corto OK
                        gpio_set_level(GPIO_LED, 1);
                        vTaskDelay(pdMS_TO_TICKS(80));
                        gpio_set_level(GPIO_LED, 0);
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        current_state = STATE_IDLE;
                        xSemaphoreGive(state_mutex);
                        ESP_LOGI(TAG, "[WAIT_BTN2->IDLE] Ciclo completo. Mantenga PB1 para nuevo test.");
                    } else if (st == STATE_SIGNAL_ON) {
                        ESP_LOGW(TAG, "PB2 presionado antes de soltar PB1 - ignorado (debe soltar PB1 primero)");
                    } else if (st == STATE_ARMED) {
                        ESP_LOGW(TAG, "PB2 presionado durante armado - ignorado");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void state_machine_task(void* arg) {
    // Esta tarea maneja timeouts y el encendido de la senal
    while (1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        system_state_t st = current_state;
        xSemaphoreGive(state_mutex);

        switch (st) {
            case STATE_ARMED: {
                int64_t elapsed_ms = (esp_timer_get_time() - t_hold_press_us) / 1000;
                if (elapsed_ms >= random_delay_ms) {
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    // doble check por si hubo false start
                    if (current_state == STATE_ARMED) {
                        current_state = STATE_SIGNAL_ON;
                        t_signal_on_us = esp_timer_get_time();
                        gpio_set_level(GPIO_LED, 1);
                        gpio_set_level(GPIO_BUZZER, 1);
                        ESP_LOGI(TAG, ">>> SENAL ON <<< LED+BUZZER - SUELTE PB1 y PRESIONE PB2 lo mas rapido!");
                        // publicar status
                        if (mqtt_connected) {
                            cJSON* r = cJSON_CreateObject();
                            cJSON_AddStringToObject(r, "status", "signal_on");
                            cJSON_AddNumberToObject(r, "random_delay_ms", random_delay_ms);
                            cJSON_AddNumberToObject(r, "timestamp", t_signal_on_us/1000);
                            mqtt_publish_json(r);
                        }
                    }
                    xSemaphoreGive(state_mutex);
                }
                break;
            }
            case STATE_SIGNAL_ON: {
                int64_t elapsed_ms = (esp_timer_get_time() - t_signal_on_us) / 1000;
                if (elapsed_ms > SIGNAL_TIMEOUT_MS) {
                    publish_timeout("wait_release");
                    gpio_set_level(GPIO_LED, 0);
                    gpio_set_level(GPIO_BUZZER, 0);
                    blink_error(2);
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    current_state = STATE_IDLE;
                    xSemaphoreGive(state_mutex);
                    ESP_LOGI(TAG, "[TIMEOUT SIGNAL->IDLE]");
                }
                break;
            }
            case STATE_WAIT_BTN2: {
                int64_t elapsed_ms = (esp_timer_get_time() - t_hold_release_us) / 1000;
                if (elapsed_ms > BUTTON2_TIMEOUT_MS) {
                    publish_timeout("wait_btn2");
                    gpio_set_level(GPIO_LED, 0);
                    gpio_set_level(GPIO_BUZZER, 0);
                    blink_error(2);
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    current_state = STATE_IDLE;
                    xSemaphoreGive(state_mutex);
                    ESP_LOGI(TAG, "[TIMEOUT WAIT_BTN2->IDLE]");
                }
                break;
            }
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void status_task(void* arg) {
    // parpadeo lento en IDLE, solido en ARMED/SIGNAL
    while (1) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        system_state_t st = current_state;
        xSemaphoreGive(state_mutex);
        if (st == STATE_IDLE) {
            // heartbeat cada 2s si IDLE
            // no interferir con blink_error; ya manejado
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP_LOGI(TAG, "Esperando... Mantenga PB1 (GPIO%d) presionado para iniciar. Estado: IDLE | MQTT:%s", GPIO_BUTTON_HOLD, mqtt_connected?"OK":"OFF");
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, " TAREA 7 - Reaction Timer - ESP32-S3");
    ESP_LOGI(TAG, " Flujo: HOLD PB1 -> delay 2-6s -> LED/BUZZER -> RELEASE PB1 -> PRESS PB2 -> MQTT");
    ESP_LOGI(TAG, " Precision: 1 ms (esp_timer 1 us)");
    ESP_LOGI(TAG, "=========================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    state_mutex = xSemaphoreCreateMutex();
    assert(state_mutex);

    gpio_init();
    wifi_init_sta();

    xTaskCreate(button_task,        "btn_task",   4096, NULL, 10, NULL);
    xTaskCreate(state_machine_task, "sm_task",    4096, NULL, 10, NULL);
    xTaskCreate(status_task,        "status_task",2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Sistema listo. Mantenga PB1 y espere la senal.");
}
