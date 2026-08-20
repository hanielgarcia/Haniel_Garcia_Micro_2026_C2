# TAREA 7 - Sistema de Medición de Reacción Humana - ESP32-S3

Sistema corregido y mejorado respecto a TAREA 6. Mide con **precisión de 1 ms** usando `esp_timer_get_time()` (resolución 1 µs).

## Flujo (enunciado)

1. Usuario **mantiene presionado PB1 (HOLD)** y espera.
2. Micro espera **delay aleatorio 2–6 s** y enciende **LED + Buzzer** = SEÑAL.
3. Usuario **suelta PB1** → se mide `release_time_ms`.
4. Usuario **presiona PB2** lo más rápido posible → se mide `reaction_time_ms`.
5. Todo se envía por **MQTT JSON** a panel en celular.

Si suelta PB1 antes de la señal → **FALSE START**.

## Correcciones vs TAREA 6

| Bug TAREA6 | Fix TAREA7 |
|---|---|
| Nunca salía de `STATE_SIGNAL_ACTIVE` (deadlock) y usaba flanco equivocado para release | Máquina de 5 estados `IDLE → ARMED → SIGNAL_ON → WAIT_BTN2` con transiciones por nivel |
| `button_task` detectaba release como `1→0` (press) | Detecta `level==1` para release, `level==0` para press, con timestamp de ISR |
| Sin antitrampa | `FALSE START` si suelta en ARMED |
| Sin debounce, timeout | Debounce 50 ms en ISR + timeouts 10 s / 5 s |
| Solo `reaction` y `hold` | Reporta 4 métricas: `release`, `reaction`, `travel`, `hold` + `random_delay` + `status` |
| MQTT solo `reaction_timer/data` | Agrega `reaction_timer/status` para señal en tiempo real |
| GPIO 0/1 conflictivos con BOOT | **GPIO 4/5** por defecto (configurables) |

## Hardware

| Componente | GPIO T7 | Nota |
|---|---|---|
| PB1 HOLD (soltar) | **GPIO 4** | pull-up interno, activo en LOW. Cambia a 0 si quieres usar BOOT |
| PB2 REACT (presionar) | **GPIO 5** | pull-up interno |
| LED señal | GPIO 2 | 220Ω a GND |
| Buzzer activo 5V | GPIO 3 | |

> Para usar GPIO 0/1 como en TAREA6 edita `#define GPIO_BUTTON_HOLD 0` en `main/reaction_timer.c`.

## Métricas MQTT (JSON) - Precisión 1 ms

```json
{
  "status": "ok",
  "random_delay_ms": 3420,
  "hold_time_ms": 3850,
  "release_time_ms": 215,
  "reaction_time_ms": 280,
  "travel_time_ms": 65,
  "button1_hold_time_ms": 3850,
  "timestamp": 1234567890,
  "timestamp_us": 1234567890123
}
```

- `release_time_ms` = `t_suelta_PB1 - t_señal` (tiempo de reacción de soltada)
- `reaction_time_ms` = `t_presiona_PB2 - t_señal` (tiempo total pedido en enunciado)
- `travel_time_ms` = `t_PB2 - t_suelta` (cuánto tardó en moverse entre botones)
- `hold_time_ms` = `t_suelta - t_presiona_PB1` (cuánto mantuvo PB1)
- `false_start`: `{"status":"false_start","hold_time_ms":123,"random_delay_ms":...}`
- `timeout`: `{"status":"timeout","stage":"wait_release"}`

Todos con `esp_timer_get_time()/1000` (64-bit, no overflow FreeRTOS tick).

## Configuración WiFi/MQTT

Edita `main/reaction_timer.c`:

```c
#define WIFI_SSID       "TU_SSID"            // 2.4 GHz
#define WIFI_PASS       "TU_PASSWORD"
#define MQTT_BROKER_URI "mqtt://test.mosquitto.org"
```

Broker público por defecto (sin auth). Para producción usa HiveMQ/EMQX con usuario/clave y TLS.

## Compilar y flashear

Usa el .bat incluido o idf.py directo:

```bat
build_flash.bat build
build_flash.bat flash          # o flash_monitor
build_flash.bat monitor
```

Manual (PowerShell con ESP-IDF exportado):
```ps
cd "C:\Users\hanie\Desktop\Haniel_Garcia_Micro_2026_C2\TAREA7"
idf.py build
idf.py -p COM3 flash monitor   # cambia COM3
```

## Panel en celular por MQTT

### Opción A - Panel web de este repo (recomendado)

En tu PC (misma WiFi que el celular):

```bat
cd mobile_panel
pip install -r requirements.txt
python app.py
# abre http://<IP-PC>:5000 en el celular
```

El servidor se suscribe a `test.mosquitto.org` y reenvía por WebSocket al celular en tiempo real. Muestra métricas, min/prom/max, historial 50, y detecta FALSE START.

### Opción B - App nativa MQTT (sin PC)

Instala en el celular:

- **Android:** *IoT MQTT Panel* o *MQTT Dashboard (HiveMQ)*
- **iOS:** *MQTT Analyzer* o *IoT MQTT Panel*

Configura:
```
Broker: test.mosquitto.org
Puerto: 1883
Topic subscribe: reaction_timer/data
Topic subscribe: reaction_timer/status
```

Verás el JSON en crudo. Crea un dashboard con 4 texts mapeados a `$.reaction_time_ms` etc.

### Opción C - MQTTX (desktop debug)

`mqtt://test.mosquitto.org:1883` → subscribe `reaction_timer/#`.

## Precisión

- Fuente: `esp_timer_get_time()` → contador 64-bit en µs (SYSTIMER del ESP32-S3).
- `CONFIG_FREERTOS_HZ=1000` → tick 1 ms, pero la medición NO usa ticks, usa `esp_timer`.
- ISR guarda `isr_time_us` con `esp_timer_get_time()` para eliminar latencia de cola.
- Reportado: `ms = us / 1000` (truncado). Error < 1 ms + jitter ISR (~5 µs).

## Estructura

```
TAREA7/
├── CMakeLists.txt
├── sdkconfig.defaults
├── build_flash.bat
├── README.md
├── main/
│   ├── CMakeLists.txt
│   └── reaction_timer.c   # firmware corregido
└── mobile_panel/
    ├── app.py             # bridge MQTT→WebSocket
    ├── requirements.txt
    └── templates/index.html  # dashboard responsive
```

## Flujo de estados (código)

```
IDLE --(PB1 press)--> ARMED --(2-6s)--> SIGNAL_ON --(PB1 release)--> WAIT_BTN2 --(PB2 press)--> IDLE
                       |                 |                           |
                       +--(release early)-> FALSE_START -> IDLE     +--(5s timeout)-> IDLE
                                         +--(10s timeout)-> IDLE
```

## Troubleshooting

| Síntoma | Causa |
|---|---|
| WiFi no conecta | Solo 2.4 GHz, SSID/pass mal, sin `idf.py menuconfig` |
| MQTT no llega | `test.mosquitto.org` caído → prueba `broker.hivemq.com` |
| Botones no responden | Revisa pull-up, wiring 3.3V, debounce |
| False start eterno | No mantengas PB1 presionado al boot |
| LED no enciende | GPIO2 en algunos S3 es RGB → prueba GPIO8 |

## Licencia

MIT - Haniel Garcia 2026
