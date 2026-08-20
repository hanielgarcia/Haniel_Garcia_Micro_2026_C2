"""
TAREA7 - Panel Movil MQTT - Reaction Timer
Suscribe a test.mosquitto.org y reenvia por SocketIO a celulares en la misma red
"""
from flask import Flask, render_template
from flask_socketio import SocketIO, emit
import paho.mqtt.client as mqtt
import json
import threading

app = Flask(__name__)
app.config['SECRET_KEY'] = 'reaction_timer_t7'
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='eventlet')

MQTT_BROKER = "test.mosquitto.org"
MQTT_PORT = 1883
MQTT_TOPIC_DATA = "reaction_timer/data"
MQTT_TOPIC_STATUS = "reaction_timer/status"

latest_data = {
    "status": "idle",
    "reaction_time_ms": 0,
    "release_time_ms": 0,
    "travel_time_ms": 0,
    "hold_time_ms": 0,
    "random_delay_ms": 0,
    "timestamp": 0
}
history = []
MAX_HISTORY = 50
mqtt_status = "disconnected"

def on_connect(client, userdata, flags, rc, properties=None):
    global mqtt_status
    print(f"[MQTT] Conectado rc={rc}")
    mqtt_status = "connected"
    client.subscribe(MQTT_TOPIC_DATA)
    client.subscribe(MQTT_TOPIC_STATUS)
    socketio.emit('mqtt_status', {"status": mqtt_status})

def on_disconnect(client, userdata, flags, rc, properties=None):
    global mqtt_status
    mqtt_status = "disconnected"
    print("[MQTT] Desconectado")
    socketio.emit('mqtt_status', {"status": mqtt_status})

def on_message(client, userdata, msg):
    global latest_data, history
    try:
        payload = msg.payload.decode()
        print(f"[MQTT] {msg.topic}: {payload}")
        data = json.loads(payload)

        # status topic
        if msg.topic == MQTT_TOPIC_STATUS:
            socketio.emit('esp_status', data)
            return

        # data topic
        latest_data = data
        # normalizar compatibilidad TAREA6 vs TAREA7
        if "button1_hold_time_ms" in data and "hold_time_ms" not in data:
            data["hold_time_ms"] = data["button1_hold_time_ms"]

        # solo agregar a historial si es ok o false_start/timeout
        hist_entry = {
            "status": data.get("status","ok"),
            "reaction_time_ms": data.get("reaction_time_ms",0),
            "release_time_ms": data.get("release_time_ms",0),
            "travel_time_ms": data.get("travel_time_ms",0),
            "hold_time_ms": data.get("hold_time_ms", data.get("button1_hold_time_ms",0)),
            "random_delay_ms": data.get("random_delay_ms",0),
            "timestamp": data.get("timestamp",0)
        }
        history.insert(0, hist_entry)
        if len(history) > MAX_HISTORY:
            history.pop()

        socketio.emit('new_data', data)
        socketio.emit('history_update', history)
    except Exception as e:
        print(f"[ERR] on_message: {e}")

def mqtt_thread():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    # reconnect logic
    while True:
        try:
            client.connect(MQTT_BROKER, MQTT_PORT, 60)
            client.loop_forever()
        except Exception as e:
            print(f"[MQTT] error conexion: {e}, reintentando en 5s...")
            import time; time.sleep(5)

@app.route('/')
def index():
    return render_template('index.html')

@socketio.on('connect')
def handle_connect():
    emit('initial_data', latest_data)
    emit('history_update', history)
    emit('mqtt_status', {"status": mqtt_status})

@socketio.on('request_history')
def handle_history_request():
    emit('history_update', history)

@socketio.on('clear_history')
def handle_clear():
    global history
    history = []
    emit('history_update', history, broadcast=True)

if __name__ == '__main__':
    t = threading.Thread(target=mqtt_thread, daemon=True)
    t.start()
    print("Panel TAREA7 en http://0.0.0.0:5000 - abre en tu celular (misma WiFi)")
    socketio.run(app, host='0.0.0.0', port=5000, debug=False)
