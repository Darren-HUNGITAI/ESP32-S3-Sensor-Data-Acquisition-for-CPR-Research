# ICM-20948 Collector GUI

Legacy note: this collector belongs to the previous MQTT-based prototype. The
current firmware path uses USB serial CSV output and does not publish MQTT
messages.

This container subscribes to the existing Mosquitto broker, stores incoming
ICM-20948 MQTT samples in SQLite, and serves a live browser GUI.

## Start

Start the existing local broker first:

```powershell
cd ..\..\local_mqtt_broker
docker compose up -d
```

Then start this collector from the firmware project:

```powershell
cd ..\esp32s3_icm20948_mqtt_demo\collector_gui
docker compose up --build
```

Open:

```text
http://localhost:8080
```

## Defaults

| Setting | Default |
| --- | --- |
| `MQTT_HOST` | `host.docker.internal` |
| `MQTT_PORT` | `1883` |
| `MQTT_TOPIC` | `esp32s3/icm20948/#` |
| `SQLITE_PATH` | `/app/data/icm20948.sqlite` |

SQLite data is stored in the Docker volume `icm20948_collector_data`.

## API

- `GET /api/health`
- `GET /api/samples?limit=500`
- `GET /api/status`
- `GET /api/summary`
- `WebSocket /ws/live`

## Manual MQTT Test

From this `collector_gui` folder, publish one valid sample through the Python
dependencies already installed in the collector container:

```powershell
docker compose exec collector-gui python -c "import json; import paho.mqtt.publish as publish; payload=json.dumps({'uptime_ms':12345,'acc_mg':{'x':1.0,'y':2.0,'z':3.0},'gyr_dps':{'x':0.1,'y':0.2,'z':0.3}}); publish.single('esp32s3/icm20948/data', payload=payload, hostname='host.docker.internal', port=1883)"
```

Send device status:

```powershell
cd ..\..\local_mqtt_broker
docker compose exec mosquitto mosquitto_pub -h localhost -t "esp32s3/icm20948/status" -m "online" -r
```
