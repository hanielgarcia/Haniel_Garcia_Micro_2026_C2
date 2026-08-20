# Reaction Timer - ESP32-S3

Sistema de medición de tiempo de reacción humana con precisión de milisegundos usando ESP-IDF v5.5.4.

## Hardware Requerido

- ESP32-S3 DevKit
- 2x Push Buttons (con resistencia pull-up externa o usar pull-up interno)
- 1x LED + resistencia 220Ω
- 1x Buzzer activo (5V)
- Jumpers y protoboard

## Conexiones GPIO

| Componente | GPIO ESP32-S3 |
|------------|---------------|
| Botón 1 (Espera/Suelta) | GPIO 0 (Boot) |
| Botón 2 (Reacción) | GPIO 1 |
| LED | GPIO 2 |
| Buzzer | GPIO 3 |

> **Nota:** GPIO 0 es el botón BOOT en la mayoría de devkits. Si lo usas, mantén presionado BOOT al iniciar para entrar en modo descarga.

## Configuración

Edita `main/reaction_timer.c` y cambia tus credenciales WiFi:

```c
#define WIFI_SSID         "TU_SSID"
#define WIFI_PASS         "TU_PASSWORD"
#define MQTT_BROKER_URI   "mqtt://test.mosquitto.org"
```

## Compilar y Flashear

```bash
# En PowerShell con ESP-IDF activado
cd C:\Users\hanie\Desktop\Haniel_Garcia_Micro_2026_C2\reaction_timer
idf.py build
idf.py -p COMX flash monitor
```

Reemplaza `COMX` por tu puerto serie (ej: `COM3`).

## Funcionamiento

1. **Inicio**: Sistema espera a que presiones **Botón 1** y lo mantengas presionado
2. **Delay aleatorio**: Después de 2-7 segundos aleatorios, se enciende **LED + Buzzer**
3. **Reacción**: Suelta **Botón 1** y presiona **Botón 2** lo más rápido posible
4. **Resultados**: Se calculan y envían por MQTT:
   - `reaction_time_ms`: Tiempo entre señal (LED/Buzzer) y Botón 2
   - `button1_hold_time_ms`: Tiempo que mantuviste Botón 1 presionado
   - `random_delay_ms`: Delay aleatorio usado
   - `timestamp`: Timestamp del evento

## MQTT Data Format (JSON)

```json
{
  "reaction_time_ms": 245,
  "button1_hold_time_ms": 3421,
  "random_delay_ms": 4123,
  "timestamp": 1699999999999
}
```

## Panel en Celular (MQTT)

### Opción 1: MQTT Explorer / MQTTX (Desktop)
Conéctate a `mqtt://test.mosquitto.org` y suscríbete a `reaction_timer/data`

### Opción 2: App Móvil MQTT
- **Android**: MQTT Dashboard, IoT MQTT Panel
- **iOS**: MQTT Analyzer, IoT MQTT Panel
- Configura broker: `test.mosquitto.org:1883`
- Topic: `reaction_timer/data`

### Opción 3: Panel Web (Python + Flask + SocketIO)
Ver `mobile_panel/` para un dashboard web en tiempo real.

## Precisión

- Usa `esp_timer_get_time()` (microsegundos, 64-bit)
- Resolución: 1 μs
- Precisión reportada: 1 ms
- FreeRTOS tick rate: 1000 Hz (1 ms)

## Estructura del Proyecto

```
reaction_timer/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   └── reaction_timer.c
��── mobile_panel/          # Panel web opcional
    ├── app.py
    ├── requirements.txt
    └── templates/
        └── index.html
```

## Troubleshooting

| Problema | Solución |
|----------|----------|
| No conecta WiFi | Verifica SSID/PASS, 2.4GHz only |
| MQTT no conecta | Broker público puede estar caído, usa otro |
| Botones no responden | Verifica pull-ups, wiring, GPIO config |
| Error flash | Mantén BOOT presionado al conectar USB |

## Licencia

MIT