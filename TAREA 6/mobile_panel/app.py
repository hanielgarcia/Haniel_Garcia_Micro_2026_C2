from flask import Flask, render_template
from flask_socketio import SocketIO, emit
import paho.mqtt.client as mqtt
import json
import threading
import time

app = Flask(__name__)
app.config['SECRET_KEY'] = 'reaction_timer_secret'
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='eventlet')

MQTT_BROKER = "test.mosquitto.org"
MQTT_PORT = 1883
MQTT_TOPIC = "reaction_timer/data"

latest_data = {
    "reaction_time_ms": 0,
    "button1_hold_time_ms": 0,
    "random_delay_ms": 0,
    "timestamp": 0
}
history = []
MAX_HISTORY = 50

def on_connect(client, userdata, flags, rc, properties=None):
    print(f"MQTT Connected with result code {rc}")
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    global latest_data, history
    try:
        data = json.loads(msg.payload.decode())
        latest_data = data
        history.insert(0, data)
        if len(history) > MAX_HISTORY:
            history.pop()
        
        socketio.emit('new_data', data)
        socketio.emit('history_update', history)
        print(f"Received: {data}")
    except json.JSONDecodeError:
        print("Invalid JSON received")

def mqtt_thread():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()

@app.route('/')
def index():
    return render_template('index.html', initial_data=latest_data, history=history)

@socketio.on('connect')
def handle_connect():
    emit('initial_data', latest_data)
    emit('history_update', history)

@socketio.on('request_history')
def handle_history_request():
    emit('history_update', history)

if __name__ == '__main__':
    mqtt_thread_obj = threading.Thread(target=mqtt_thread, daemon=True)
    mqtt_thread_obj.start()
    
    print("Starting web server on http://0.0.0.0:5000")
    socketio.run(app, host='0.0.0.0', port=5000, debug=False)