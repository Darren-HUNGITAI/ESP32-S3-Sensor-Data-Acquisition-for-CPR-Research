from __future__ import annotations

import os


class Settings:
    mqtt_host: str = os.getenv("MQTT_HOST", "host.docker.internal")
    mqtt_port: int = int(os.getenv("MQTT_PORT", "1883"))
    mqtt_topic: str = os.getenv("MQTT_TOPIC", "esp32s3/icm20948/#")
    mqtt_client_id: str = os.getenv("MQTT_CLIENT_ID", "icm20948-collector-gui")
    sqlite_path: str = os.getenv("SQLITE_PATH", "/app/data/icm20948.sqlite")


settings = Settings()
