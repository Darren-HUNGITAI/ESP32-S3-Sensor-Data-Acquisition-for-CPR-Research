# ESP32-S3 ICM-20948 Feasibility Logger

This project currently validates the A401 + MCP6002 pressure front end with the
ESP32-S3 internal ADC and prints CSV samples over USB serial. The current
feasibility phase intentionally does not use MQTT or a broker.

The PC side can display the serial stream with PlatformIO Serial Monitor. This
keeps the first validation focused on sensor wiring, ADC stability, and whether
pressing A401 produces a useful waveform.

An optional Wi-Fi UDP transport is available for the next experiment phase when
the ESP32-S3 should stream low-latency waveform data directly to the local PC.
It uses CSV packets by default, so the same variables can be captured through
either serial or UDP.

## Data Paths

Current proof-of-concept path:

```text
A401 -> MCP6002 analog front end -> ESP32-S3 internal ADC -> USB serial CSV
```

Optional low-latency wireless path:

```text
A401 -> MCP6002 analog front end -> ESP32-S3 internal ADC -> Wi-Fi UDP CSV
```

Default firmware output:

- Baud rate: `921600`
- Sample interval: `10000 us`, about `100 Hz`
- Format: CSV
- Units: pressure ADC raw counts and millivolts
- ICM-20948 output is temporarily disabled for pressure-circuit validation.

CSV columns:

```text
uptime_ms
pressure_vout_raw
pressure_vout_mv
pressure_vref_raw
pressure_vref_mv
pressure_delta_mv
```

Lines starting with `#` are status comments.

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

The ICM-20948 DMP is not enabled in this firmware. The device outputs raw
accelerometer and gyroscope samples.

## A401 + MCP6002 Internal ADC Wiring

This wiring is the first bench validation path for the parts currently on hand.
It uses the ESP32-S3 internal ADC instead of an external MCP3202.

### MCP6002-I/P pinout

| MCP6002 pin | Connection |
| --- | --- |
| 1 OUTA | `VREF_BUF`, also ESP32-S3 GPIO2 |
| 2 -INA | pin 1 OUTA |
| 3 +INA | resistor divider midpoint `VREF_RAW` |
| 4 VSS | GND |
| 5 +INB | `VREF_BUF` |
| 6 -INB | A401 signal node and feedback resistor |
| 7 OUTB | `VOUT_PRESSURE`, also ESP32-S3 GPIO1 |
| 8 VDD | 3V3 |

### VREF divider and pressure front end

| Part | Connection |
| --- | --- |
| 47k resistor | 3V3 to `VREF_RAW` |
| 10k resistor | `VREF_RAW` to GND |
| 100nF capacitor | `VREF_RAW` to GND |
| 100nF capacitor | MCP6002 pin 8 to GND, close to the IC |
| A401 | MCP6002 pin 6 to GND |
| 47k feedback resistor | MCP6002 pin 7 to pin 6 |
| ESP32-S3 GPIO1 | MCP6002 pin 7 `VOUT_PRESSURE` |
| ESP32-S3 GPIO2 | MCP6002 pin 1 `VREF_BUF` |

With 47k over 10k, `VREF_RAW` is about `0.58 V` from a 3.3 V supply. The
firmware reports both `pressure_vout_mv` and `pressure_vref_mv`; use
`pressure_delta_mv` as the first pressure trend signal. Pressing A401 should
make `pressure_vout_mv` and `pressure_delta_mv` increase.

## Build And Upload

Default USB serial firmware:

```powershell
platformio run -e esp32s3_n16r8
platformio run -e esp32s3_n16r8 --target upload
```

Open the serial monitor at the same baud rate:

```powershell
platformio device monitor --baud 921600
```

Watch `pressure_delta_mv`. Pressing A401 should make it increase.

## Save Samples On The PC

The default firmware output is CSV and can be captured with these scripts.

Install the Python serial dependency:

```powershell
python -m pip install -r tools\requirements.txt
```

List serial ports:

```powershell
python tools\serial_to_csv.py --list-ports
```

Capture data, replacing `COM6` with the ESP32-S3 port:

```powershell
python tools\serial_to_csv.py --port COM6 --out data\trial_001.csv
```

Stop with `Ctrl+C`. The output CSV can then be opened in Excel, Python,
MATLAB, or another analysis tool.

## Optional Wi-Fi UDP Streaming

Use this when the test needs the shortest practical direct-to-PC wireless
latency and real-time waveform transfer without MQTT. UDP avoids broker and TCP
retry latency.

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Set `WIFI_SSID`, `WIFI_PASSWORD`, and `UDP_TARGET_IP` in `include/secrets.h`.
   Use the PC IPv4 address from `ipconfig`.
3. Start the PC receiver:

```powershell
python tools\udp_to_csv.py --port 5005 --out data\udp_trial_001.csv
```

4. Build and upload the UDP firmware:

```powershell
platformio run -e esp32s3_n16r8_udp
platformio run -e esp32s3_n16r8_udp --target upload
```

If Windows Firewall prompts for Python UDP access, allow it on the local
network used by the ESP32-S3 and PC.

## Future Wireless Direction

The professor's latest direction is to avoid MQTT during feasibility validation,
but keep the architecture ready for a lower-latency wireless path that streams
waveform data or transfers captured files to a local PC.

Practical next candidates:

1. USB serial logger for the current proof-of-concept.
2. Wi-Fi UDP sender to a local PC for direct low-latency real-time waveform
   streaming without extra hardware.
3. Wi-Fi TCP sender if exact file delivery matters more than latency.
4. ESP-NOW for very low ESP32-to-ESP32 latency, with a receiver ESP32 connected
   to the PC over USB. A normal PC Wi-Fi adapter usually cannot receive ESP-NOW
   frames directly.

For the next stage, keep the sample names stable and swap only the transport:

```text
sample -> encode CSV/binary record -> transport -> PC viewer or file writer
```

This makes it possible to compare USB serial, Wi-Fi UDP/TCP, ESP-NOW, or any
other candidate using the same pressure variables and timestamp measurements.

## Legacy MQTT Files

The `collector_gui/` folder is the previous MQTT-based collector and browser
GUI. It is kept for reference, but it is not used by the current firmware path.
