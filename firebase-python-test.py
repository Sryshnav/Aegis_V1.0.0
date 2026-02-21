import paho.mqtt.client as mqtt
import firebase_admin
from firebase_admin import credentials, db
import random
import ssl

# ==========================================
# 1. FIREBASE CONFIGURATION
# ==========================================
cred = credentials.Certificate("serviceAccountKey.json")
firebase_admin.initialize_app(cred, {
    'databaseURL': 'https://aegisreal-8b7a3-default-rtdb.firebaseio.com'
})

# ==========================================
# 2. MQTT CONFIGURATION (SECURE)
# ==========================================
MQTT_BROKER = "broker.hivemq.com"
MQTT_PORT = 8883
MQTT_TOPIC_TELE = "crabs/telematics/live"
MQTT_TOPIC_CMD = "crabs/commands/immo"
MQTT_TOPIC_RATE = "crabs/commands/rate"
MQTT_TOPIC_GEO = "crabs/commands/geofence"
MQTT_TOPIC_ALERT = "crabs/alerts/geofence"

# --- FIREBASE LISTENERS ---
def immo_stream_handler(event):
    if event.data is not None:
        print(f"\n🔔 IMMO COMMAND FROM CLOUD: {event.data}")
        if str(event.data).lower() == 'true':
            client.publish(MQTT_TOPIC_CMD, "1")
        else:
            client.publish(MQTT_TOPIC_CMD, "0")

def rate_stream_handler(event):
    if event.data is not None:
        print(f"\n⏱️ RATE CONFIG SIGNAL: Set to {event.data} ms")
        client.publish(MQTT_TOPIC_RATE, str(event.data))

def geo_stream_handler(event):
    # Detect if dashboard cleared the geofence
    if event.data is None or str(event.data).strip() == "":
        print("\n🛡️ EDGE COMMAND: Clearing Geofence & Disarming Alarm")
        client.publish(MQTT_TOPIC_GEO, "POLY:CLEAR", retain=True)
    else:
        poly_string = f"POLY:{event.data}"
        print(f"\n🗺️ EDGE COMMAND: Pushing New Geofence -> {poly_string}")
        client.publish(MQTT_TOPIC_GEO, poly_string, retain=True)

# ==========================================
# 3. SECURE MQTT CALLBACKS
# ==========================================
def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print("✅ SUCCESS: Bridge Connected to Secure MQTTS Cloud")
        client.subscribe(MQTT_TOPIC_TELE)
        client.subscribe(MQTT_TOPIC_ALERT)
    else:
        print(f"❌ MQTT Connection Failed: {reason_code}")

def on_message(client, userdata, msg):
    # --- HARDWARE ALERT SYNC ---
    if msg.topic == MQTT_TOPIC_ALERT:
        if msg.payload.decode('utf-8') == "BREACH":
            print("\n🚨 HARDWARE OVERRIDE: Geofence Breached! Syncing Dashboard Switch...")
            db.reference('CRABS_vehicle_live/vehicle_commands').update({'immo_active': True})
        return

    # --- STANDARD TELEMETRY PARSING ---
    payload = msg.payload.decode('utf-8')
    try:
        parts = payload.split(',')
        if len(parts) >= 20:
            parsed_data = {
                "frame_number": int(parts[3]),
                "network_operator": "Airtel" if parts[4] == "03" else ("Jio" if parts[4] == "04" else parts[4]),
                "signal_strength": int(parts[5]),
                "latitude": float(parts[9]) / 1000000.0,
                "longitude": float(parts[11]) / 1000000.0,
                "speed_kmh": float(parts[15]) / 100.0,
                "ignition": "ON" if parts[16] == "1" else "STANDBY",
                "immobilizer": "LOCKED" if parts[17] == "1" else "RELEASED",
                "battery_v": float(parts[18]) / 10.0,
                "system_status": "AWAKE"
            }

            db.reference('CRABS_vehicle_live').update(parsed_data)
            print(f"🚀 Sync [Frame {parsed_data['frame_number']}]: Spd: {parsed_data['speed_kmh']} | Lat: {parsed_data['latitude']:.6f} | Immo: {parsed_data['immobilizer']}")
    except Exception as e:
        pass

# ==========================================
# 4. EXECUTION
# ==========================================
print("Initializing CRABS Secure Bi-Directional Backend...")
client_id = f"CRABS-Bridge-{random.randint(1000, 9999)}"
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
client.tls_set(cert_reqs=ssl.CERT_NONE)
client.on_connect = on_connect
client.on_message = on_message
client.connect(MQTT_BROKER, MQTT_PORT, 60)

print("🛰️ Opening Firebase Pipes...")
db.reference('CRABS_vehicle_live/vehicle_commands/immo_active').listen(immo_stream_handler)
db.reference('CRABS_vehicle_live/vehicle_commands/update_rate_ms').listen(rate_stream_handler)
db.reference('CRABS_vehicle_live/vehicle_commands/geofence_polygon').listen(geo_stream_handler)

try:
    client.loop_forever()
except KeyboardInterrupt:
    print("\nBridge Offline.")