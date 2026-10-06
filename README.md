# 💧 Smart Water Tank Controller (ESP32 + HC-SR04 + MQTT)

A real-time, robust Smart Water Tank Controller and Monitoring System powered by ESP32, HC-SR04 Ultrasonic Sensor, Active LOW Relay, and Mosquitto MQTT Broker. Includes an ultra-responsive, minimal, and modern Web Dashboard hosted on Ubuntu Server and accessible via `https://www.rahat.eu.cc/`.

---

## 🚀 Key Features

- **Unchanged Core Algorithms & Mathematical Filtering**:
  - **0.5s Fast Sampling** with HC-SR04 ultrasonic sensor.
  - **2× Cascaded Low-Pass Filter (LPF)** to suppress transient spikes ($\alpha_1 = 0.35, \alpha_2 = 0.25$).
  - **5.0s Statistical Calculation Window** using **Median + Median Absolute Deviation (MAD)** robust mean to completely reject sensor noise and turbulence.
  - **Dual Hysteresis with Continuous Verification**:
    - **Motor ON**: Triggers when water $\le$ `motorOnPercent` (default 25%) and verifies stability over a **20-second confirmation buffer**.
    - **Motor OFF**: Triggers when water $\ge$ `motorOffPercent` (default 75%) and verifies stability over a **10-second confirmation buffer**.
    - Auto-cancels if water level bounces back during verification.
  - **10-Second Sensor Calibration**: For Empty Tank and Full Tank distance calibration with EEPROM/Preferences persistence.
  - **Water Usage Calculation**: Tracks cumulative water consumption in Liters.
- **Bi-directional Realtime MQTT**:
  - Publishes full telemetry JSON to `devices/ESP32_WATER_01/telemetry`.
  - Subscribes to control commands on `devices/ESP32_WATER_01/command`.
  - Supports Manual Motor ON, Manual Motor OFF, AUTO mode, Settings Update, Calibration, and Factory Reset over MQTT.
- **Dual Web Architecture**:
  - **Hosted Web Dashboard**: Hosted at `https://www.rahat.eu.cc/` with live MQTT over Secure WebSockets (`wss://www.rahat.eu.cc/mqtt`).
  - **Embedded ESP32 Web Server**: Built-in HTTP server (`http://192.168.4.1` in AP mode or local STA IP) for standalone local operation.

---

## 📌 Hardware Pin Connections

| ESP32 Pin | Component Pin | Description |
|---|---|---|
| **GPIO 5** | HC-SR04 **TRIG** | Ultrasonic Trigger pulse |
| **GPIO 18** | HC-SR04 **ECHO** | Ultrasonic Echo pulse |
| **GPIO 25** | Relay **IN** | Motor control relay (Active LOW: `LOW = ON`, `HIGH = OFF`) |
| **VCC (5V / VIN)** | HC-SR04 & Relay VCC | 5V Power Supply |
| **GND** | HC-SR04 & Relay GND | Common Ground |

---

## 📡 MQTT Configuration & Credentials

| Parameter | Value |
|---|---|
| **Broker IP (Host)** | `192.168.110.133` |
| **Port (TCP / ESP32)** | `1883` |
| **Port (WebSocket / Web)**| `9001` (Proxied via Apache at `wss://www.rahat.eu.cc/mqtt`) |
| **MQTT Username** | `rahat300809` |
| **MQTT Password** | `RAHAT678` |
| **Client ID (ESP32)** | `ESP32_WATER_01` |
| **Telemetry Topic (Data)**| `devices/ESP32_WATER_01/telemetry` |
| **Command Topic (Control)**| `devices/ESP32_WATER_01/command` |

### Telemetry Payload Format (JSON)
Published every 5 seconds, upon state changes, and every 1 second during calibration/verification:
```json
{
  "distance": 45.20,
  "level": 54.80,
  "percent": 54.8,
  "liters": 430.50,
  "capacity": 785.40,
  "used": 12.30,
  "radius": 50.0,
  "height": 100.0,
  "on": 25.0,
  "off": 75.0,
  "offset": 0.0,
  "empty": 100.0,
  "full": 10.0,
  "calibrated": true,
  "calibrationRunning": false,
  "relay": false,
  "motor": false,
  "mode": "AUTO",
  "state": "NORMAL",
  "remaining": 0,
  "calType": "",
  "progress": 0.0
}
```

### Command Payloads Supported
Publish JSON to `devices/ESP32_WATER_01/command`:
- **Motor Control**:
  - `{"action": "motor", "state": "on"}` (Manual ON)
  - `{"action": "motor", "state": "off"}` (Manual OFF)
  - `{"action": "motor", "state": "auto"}` (Auto mode)
- **Save Settings**:
  - `{"action": "save", "radius": 50.0, "height": 100.0, "on": 25.0, "off": 75.0, "offset": 0.0}`
- **Sensor Calibration**:
  - `{"action": "calibrate", "type": "empty"}`
  - `{"action": "calibrate", "type": "full"}`
- **Reset Water Usage**:
  - `{"action": "resetUsage"}`
- **Reset All Settings**:
  - `{"action": "resetAll"}`
- **Request Immediate Telemetry Status**:
  - `{"action": "status"}`

---

## 📁 Repository Directory Structure

```text
water_tank_indecator/
├── ESP32_Water_Controller/
│   └── ESP32_Water_Controller.ino    # Arduino sketch for ESP32 with MQTT & WebServer
├── web/
│   ├── index.html                    # Modern, minimal responsive web dashboard
│   ├── style.css                     # Premium styling with smooth transitions & responsive grid
│   ├── app.js                        # MQTT WebSocket real-time engine & fluid tank animation
│   └── mqtt.min.js                   # Local MQTT.js bundle for zero-external-dependency offline support
├── README.md                         # Documentation
└── .gitignore                        # Git ignore rules
```

---

## 🛠️ Arduino IDE Setup Instructions

1. Install the **ESP32 Board Package** in Arduino IDE (Boards Manager -> `esp32` by Espressif).
2. Install the **PubSubClient** library:
   - Go to **Sketch** -> **Include Library** -> **Manage Libraries...**
   - Search for `PubSubClient` by Nick O'Leary and click **Install**.
3. Open `ESP32_Water_Controller/ESP32_Water_Controller.ino`.
4. Verify your WiFi credentials:
   ```cpp
   const char* WIFI_SSID = "IoT Lab";
   const char* WIFI_PASS = "iot@diu123";
   ```
5. Select your ESP32 board (e.g. `ESP32 Dev Module`) and COM Port.
6. Click **Upload**.
7. Open the Serial Monitor at **115200 baud** to view real-time logs, WiFi connection, and MQTT status.

---

## 🌐 Web Dashboard Deployment on Ubuntu Server

The web dashboard is hosted at `/var/www/rahat` on the Ubuntu server (`192.168.110.133`) and exposed via Cloudflare Tunnel:
- **Public URL**: `https://www.rahat.eu.cc/`
- **Apache VirtualHost**: Proxies `/mqtt` directly to `ws://127.0.0.1:9001/` for seamless WebSocket communication without mixed content issues:
  ```apache
  ProxyPass /mqtt ws://127.0.0.1:9001/
  ProxyPassReverse /mqtt ws://127.0.0.1:9001/
  ```
- **Local Access**: Accessible at `http://192.168.110.133/` on the local LAN.
