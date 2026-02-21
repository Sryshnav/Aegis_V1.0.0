#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <PubSubClient.h>

const char* ssid = "airtel eduk my**e🤡"; 
const char* password = "shawn123";
const char* mqtt_server = "broker.hivemq.com";
const char* tele_topic = "crabs/telematics/live";
const char* cmd_topic = "crabs/commands/immo";
const char* rate_topic = "crabs/commands/rate";
const char* geo_topic = "crabs/commands/geofence"; 
const char* alert_topic = "crabs/alerts/geofence"; 
const char* sleep_topic = "crabs/commands/sleep";

WiFiClientSecure espClient; 
PubSubClient client(espClient);

const int IMMO_RELAY_PIN = D0;
unsigned long frameNumber = 0;


#define MAX_POLY_POINTS 10
struct Point { double lat; double lon; };
Point geofence[MAX_POLY_POINTS];
int geofenceSize = 0;
bool geofenceActive = false;
bool geofenceViolation = false;


double currentLat = 10.060000;
double currentLon = 76.620000;
unsigned long lastGpsUpdate = 0;

bool isInsidePolygon(double testLat, double testLon) {
  if (geofenceSize < 3) return true; 
  bool c = false;
  for (int i = 0, j = geofenceSize - 1; i < geofenceSize; j = i++) {
    if ( ((geofence[i].lon > testLon) != (geofence[j].lon > testLon)) &&
         (testLat < (geofence[j].lat - geofence[i].lat) * (testLon - geofence[i].lon) / (geofence[j].lon - geofence[i].lon) + geofence[i].lat) ) {
       c = !c;
    }
  }
  return c;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == cmd_topic) {
    if (msg == "1") {
      digitalWrite(IMMO_RELAY_PIN, HIGH);
      Serial1.println("LOCK"); 
      Serial.println("🛑 CLOUD COMMAND: LOCKED");
    } else {
      digitalWrite(IMMO_RELAY_PIN, LOW);
      Serial1.println("UNLOCK");
      geofenceViolation = false; 
      Serial.println("🟢 CLOUD COMMAND: UNLOCKED");
    }
  } else if (String(topic) == rate_topic) {
    Serial1.println("RATE:" + msg);
  } else if (String(topic) == sleep_topic) {
    Serial1.println("HEARTBEAT:" + msg); // PHASE 5: Forward to Sender Node
    Serial.println("⏱️ CLOUD COMMAND: Sleep Interval set to " + msg + "s");
  } else if (String(topic) == geo_topic) {
    if (msg.startsWith("POLY:")) {
      msg.replace("POLY:", "");
      geofenceSize = 0;
      int startIdx = 0;
      while (startIdx < msg.length() && geofenceSize < MAX_POLY_POINTS) {
        int semiIdx = msg.indexOf(';', startIdx);
        if (semiIdx == -1) semiIdx = msg.length();
        String pair = msg.substring(startIdx, semiIdx);
        int commaIdx = pair.indexOf(',');
        if (commaIdx > 0) {
          geofence[geofenceSize].lat = pair.substring(0, commaIdx).toDouble();
          geofence[geofenceSize].lon = pair.substring(commaIdx + 1).toDouble();
          geofenceSize++;
        }
        startIdx = semiIdx + 1;
      }
      geofenceActive = (geofenceSize >= 3);
      Serial.println("🗺️ EDGE MEMORY: Saved Geofence (" + String(geofenceSize) + " points)");
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, D7, D6); 
  pinMode(IMMO_RELAY_PIN, OUTPUT);
  digitalWrite(IMMO_RELAY_PIN, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWi-Fi Connected!");

  espClient.setInsecure();
  client.setServer(mqtt_server, 8883); 
  client.setCallback(callback);
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "Gateway-" + String(random(0, 0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      client.subscribe(cmd_topic);
      client.subscribe(rate_topic);
      client.subscribe(geo_topic);
      client.subscribe(sleep_topic); 
      Serial.println("MQTT Connected & Subscribed");
    } else { delay(5000); }
  }
}

String calculateXOR(String data) {
  byte crc = 0;
  for (int i = 0; i < data.length(); i++) crc ^= data[i];
  char hexString[3]; sprintf(hexString, "%02X", crc);
  return String(hexString);
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  if (Serial1.available() > 0) {
    String incoming = Serial1.readStringUntil('\n');
    incoming.trim();
    
    int c1 = incoming.indexOf(','), c2 = incoming.indexOf(',', c1+1), c3 = incoming.lastIndexOf(',');
    if (c1 > 0 && c2 > 0 && c3 > 0) {
      String ign = incoming.substring(0, c1);
      String voltStr = incoming.substring(c1+1, c2);
      String spdStr = incoming.substring(c2+1, c3);

      frameNumber++; 
      
      
      float speedFloat = spdStr.toFloat();
      if (millis() - lastGpsUpdate > 1000) { 
        if (speedFloat > 0 && !geofenceViolation) {
           currentLat += (speedFloat * 0.000005); 
           currentLon += (speedFloat * 0.000005);
        }
        lastGpsUpdate = millis();

        
        if (geofenceActive && !geofenceViolation) {
          if (!isInsidePolygon(currentLat, currentLon)) {
            geofenceViolation = true;
            digitalWrite(IMMO_RELAY_PIN, HIGH); 
            Serial1.println("LOCK");            
            Serial.println("🚨 EDGE ALERT: GEOFENCE BREACH! ENGINE KILLED!");
            
            client.publish(alert_topic, "BREACH"); 
          }
        }
      }

      int immoStatus = digitalRead(IMMO_RELAY_PIN);
      long rssi = WiFi.RSSI();
      int sigStrength = constrain(map(rssi, -100, -50, 0, 31), 0, 31);
      
      int scaledSpd = spdStr.toInt() * 100;
      int scaledVolt = (int)(voltStr.toFloat() * 10.0);
      String scaledLat = String((int)(currentLat * 1000000));
      String scaledLon = String((int)(currentLon * 1000000));
      String scaledHDOP = "120";
      String scaledPDOP = "150";

      String imei = "867530900112233", mcc = "404", mnc = "20";
      String ts = "1708450000";
      
      String payload = imei + ",1," + String(frameNumber) + ",03," + String(sigStrength) + "," + mcc + "," + mnc + ",1," + scaledLat + ",0," + scaledLon + ",0," + scaledHDOP + "," + scaledPDOP + "," + String(scaledSpd) + "," + ign + "," + String(immoStatus) + "," + String(scaledVolt) + "," + ts;
      String dataForCRC = String(payload.length()) + "," + payload;
      String finalPacket = "$" + dataForCRC + "," + calculateXOR(dataForCRC) + "*";
      
      Serial.println("Uplink: " + finalPacket);
      client.publish(tele_topic, finalPacket.c_str());
    }
  }
}
