# ESP32-S3 ICM-20948 MQTT Demo

This project reads the ICM-20948 over SPI and publishes JSON samples to MQTT.

Wi-Fi is managed by this firmware with up to 4 saved slots in ESP32 Preferences.
MQTT settings are also stored in ESP32 Preferences.

## Wi-Fi Storage Model

Wi-Fi credentials are stored in the ESP32 flash/NVS namespace `wifi_multi`:

```text
ssid0/pass0
ssid1/pass1
ssid2/pass2
ssid3/pass3
```

Slot order is priority order. Slot 0 is tried first, then slot 1, slot 2, and
slot 3.

Optional preload values can be compiled into `include/secrets.h`:

```cpp
#define PRELOAD_WIFI_0_SSID "YOUR_PRIMARY_WIFI_SSID"
#define PRELOAD_WIFI_0_PASSWORD "YOUR_PRIMARY_WIFI_PASSWORD"
```

Any non-empty `PRELOAD_WIFI_n_SSID` overwrites the matching Preferences slot on
boot. This is useful for preloading a demo venue Wi-Fi before arriving.

## Boot Flow

1. Initialize ICM-20948 over SPI.
2. Load MQTT settings from Preferences namespace `mqtt`.
3. Load the 4 Wi-Fi slots from Preferences namespace `wifi_multi`.
4. Apply non-empty Wi-Fi preload slots from `secrets.h`.
5. Scan nearby Wi-Fi networks.
6. Try visible stored slots in priority order.
7. Try non-visible stored slots once, so hidden SSIDs can still work.
8. If Wi-Fi or MQTT settings are missing or Wi-Fi cannot connect, open setup mode.

## ICM-20948 SPI Wiring

| ESP32-S3 | ICM-20948 |
| --- | --- |
| GPIO10 | NCS |
| GPIO11 | SDA |
| GPIO12 | SCL |
| GPIO13 | AD0 |
| 3V3 | VCC |
| GND | GND |

On this 10-pin ICM-20948 module, the SPI signals share the I2C-labeled pins:
`SCL` is SPI clock, `SDA` is SPI MOSI/SDI, `AD0` is SPI MISO/SDO, and `NCS`
is chip select. Leave `EDA`, `ECL`, `INT`, and `FSYNC` disconnected for this
firmware.

The ICM-20948 DMP is not enabled in this firmware. The device publishes raw
accelerometer, gyroscope, magnetometer, and temperature samples.

## Setup Mode

The ESP32 starts an open access point:

```text
SSID: ICM20948-Setup
Password: none
```

Connect your phone or computer to it and open:

```text
http://192.168.4.1
```

Open the parameters page and fill in:

```text
Wi-Fi slot 0 SSID/password
Wi-Fi slot 1 SSID/password
Wi-Fi slot 2 SSID/password
Wi-Fi slot 3 SSID/password
MQTT host or IP
MQTT port
MQTT username
MQTT password
MQTT TLS? 0=no, 1=yes
```

After saving, the ESP32 stores the values and restarts.

## Tailscale Funnel MQTT Demo

For the current Funnel setup, use:

```text
MQTT host or IP: your *.ts.net Funnel hostname
MQTT port: 10000
MQTT TLS? 1
```

The MQTT connection is MQTT over TLS/TCP from the ESP32 to Tailscale Funnel.
Funnel then forwards to the local Mosquitto broker.

## Reset Settings

While the firmware is running, hold the ESP32-S3 BOOT button for 5 seconds. The
device clears:

```text
wifi_multi Preferences
mqtt Preferences
ESP32 core saved Wi-Fi settings
```

Then it restarts into setup mode. Non-empty Wi-Fi preload slots in `secrets.h`
will be applied again after restart.

## Topics

- `esp32s3/icm20948/status`: retained online/offline status
- `esp32s3/icm20948/data`: JSON IMU samples, about one sample every 100 ms

## Collector GUI Container

This repo includes an optional collector and browser GUI in `collector_gui/`.
It runs as one Docker container, subscribes to the existing Mosquitto broker at
`host.docker.internal:1883`, stores samples in SQLite, and serves the GUI at:

```text
http://localhost:8080
```

Start the existing broker first:

```powershell
cd C:\Users\a0919\OneDrive\桌面\ICM-20948\local_mqtt_broker
docker compose up -d
```

Then start the collector:

```powershell
cd C:\Users\a0919\OneDrive\桌面\ICM-20948\esp32s3_icm20948_mqtt_demo\collector_gui
docker compose up --build
```

The collector exposes:

- `GET /api/health`
- `GET /api/samples?limit=500`
- `GET /api/status`
- `GET /api/summary`
- `WebSocket /ws/live`

## Subscribe From Broker Folder

```powershell
cd C:\Users\a0919\OneDrive\桌面\ICM-20948\local_mqtt_broker
docker compose exec mosquitto mosquitto_sub -h localhost -t "esp32s3/icm20948/#" -v
```

## Notes

- ESP32-S3 usually needs a 2.4 GHz Wi-Fi network. A `5G` SSID may fail if it is
  truly 5 GHz only.
- Wi-Fi and MQTT passwords are stored in ESP32 flash/NVS. This is acceptable for
  a demo, but production hardware should consider flash encryption.
- `MQTT TLS? 1` uses `WiFiClientSecure::setInsecure()`, so traffic is encrypted
  but the server certificate is not verified.
