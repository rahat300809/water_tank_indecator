# 💧 Smart Water Tank Controller — Configuration Template

Template configuration for new devices and brokers. Replace placeholders with your own values.

---

## 📡 1. Wi-Fi Configuration

| Type | SSID / Network Name | Password | Description |
|---|---|---|---|
| **Primary Wi-Fi** | `<YOUR_WIFI_SSID>` | `<YOUR_WIFI_PASSWORD>` | Main Home Router network |
| **Fallback Wi-Fi** | `<FALLBACK_SSID>` | `<FALLBACK_PASSWORD>` | Secondary backup network |
| **ESP32 AP Hotspot** | `ESP32_Water_Setup` | `12345678` | Captive portal setup hotspot |
| **Hotspot Portal URL** | `http://192.168.4.1` | — | Open in browser to reconfigure Wi-Fi |

---

## 🔐 2. MQTT Broker Configuration

| Parameter | Example Value | Details |
|---|---|---|
| **MQTT Username** | `<YOUR_MQTT_USERNAME>` | Configured in Mosquitto |
| **MQTT Password** | `<YOUR_MQTT_PASSWORD>` | Configured in Mosquitto |
| **Client ID (ESP32)** | `ESP32_WATER_01` | Unique ID per device |
| **Telemetry Topic** | `devices/<CLIENT_ID>/telemetry` | Device publishes live sensor data |
| **Command Topic** | `devices/<CLIENT_ID>/command` | Dashboard sends control commands |

---

## 🌐 3. Network Ports & Forwarding

| Service | Port | Protocol | Description |
|---|---|---|---|
| **MQTT TCP** | `1883` | TCP | Standard MQTT port for ESP32 and microcontrollers |
| **MQTT WebSocket** | `9001` | WS / WSS | WebSocket port for Web & Mobile Dashboard |
| **Web UI** | `80` / `443` | HTTP / HTTPS | Dashboard access |

---

## 📌 4. Hardware Pin Mapping (ESP32)

| ESP32 GPIO | Hardware Pin | Function |
|---|---|---|
| **GPIO 5** | HC-SR04 **TRIG** | Ultrasonic Trigger pulse |
| **GPIO 18** | HC-SR04 **ECHO** | Ultrasonic Echo return pulse |
| **GPIO 25** | Relay **IN** | Active LOW (`LOW = Motor ON`, `HIGH = Motor OFF`) |
| **VIN / 5V** | Sensor & Relay VCC | 5V Power Supply |
| **GND** | Sensor & Relay GND | Common Ground |
