from __future__ import annotations

import asyncio
import logging
import threading
from collections.abc import Callable
from typing import Any

import paho.mqtt.client as mqtt

from .config import settings
from .storage import Storage

logger = logging.getLogger(__name__)

LiveCallback = Callable[[dict[str, Any]], None]


class MqttCollector:
    def __init__(self, storage: Storage, publish_live: LiveCallback) -> None:
        self.storage = storage
        self.publish_live = publish_live
        self.connected = False
        self.last_error: str | None = None
        self._lock = threading.Lock()
        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=settings.mqtt_client_id,
        )
        self._client.on_connect = self._on_connect
        self._client.on_disconnect = self._on_disconnect
        self._client.on_message = self._on_message

    def start(self) -> None:
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        self._client.connect_async(settings.mqtt_host, settings.mqtt_port, keepalive=30)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "connected": self.connected,
                "last_error": self.last_error,
                "host": settings.mqtt_host,
                "port": settings.mqtt_port,
                "topic": settings.mqtt_topic,
            }

    @staticmethod
    def _reason_failed(reason_code: mqtt.ReasonCode) -> bool:
        is_failure = getattr(reason_code, "is_failure", None)
        if isinstance(is_failure, bool):
            return is_failure
        value = getattr(reason_code, "value", None)
        if isinstance(value, int):
            return value != 0
        return str(reason_code).lower() not in {"success", "normal disconnection"}

    def _emit(self, message: dict[str, Any]) -> None:
        try:
            self.publish_live(message)
        except RuntimeError:
            logger.exception("Failed to publish live message")

    def _on_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del userdata, flags, properties
        success = not self._reason_failed(reason_code)
        with self._lock:
            self.connected = success
            self.last_error = None if success else str(reason_code)

        if success:
            client.subscribe(settings.mqtt_topic, qos=0)
            event = self.storage.insert_event(
                "info",
                f"Connected to MQTT broker and subscribed to {settings.mqtt_topic}",
            )
            self._emit({"type": "collector_event", "event": event, "mqtt": self.snapshot()})
        else:
            event = self.storage.insert_event(
                "error",
                f"MQTT connection failed: {reason_code}",
            )
            self._emit({"type": "collector_event", "event": event, "mqtt": self.snapshot()})

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, disconnect_flags, properties
        failed = self._reason_failed(reason_code)
        with self._lock:
            self.connected = False
            self.last_error = None if not failed else str(reason_code)

        event = self.storage.insert_event(
            "warning",
            f"Disconnected from MQTT broker: {reason_code}",
        )
        self._emit({"type": "collector_event", "event": event, "mqtt": self.snapshot()})

    def _on_message(
        self,
        client: mqtt.Client,
        userdata: Any,
        message: mqtt.MQTTMessage,
    ) -> None:
        del client, userdata
        topic = message.topic
        payload_text = message.payload.decode("utf-8", errors="replace")

        if topic.endswith("/status"):
            status = self.storage.insert_status(topic, payload_text)
            self._emit({"type": "device_status", "status": status})
            return

        if topic.endswith("/data"):
            decoded = self.storage.decode_sample_payload(topic, payload_text)
            if decoded is None:
                self._emit({"type": "collector_event", "event": self.storage.summary()["latest_event"]})
                return

            sample = self.storage.insert_sample(topic, decoded, payload_text)
            self._emit({"type": "imu_sample", "sample": sample})
            return

        event = self.storage.insert_event(
            "info",
            "Ignored non-data MQTT message",
            topic=topic,
            raw_payload=payload_text,
        )
        self._emit({"type": "collector_event", "event": event})


class LiveHub:
    def __init__(self) -> None:
        self._clients: set[asyncio.Queue[dict[str, Any]]] = set()
        self._lock = threading.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None

    def bind_loop(self, loop: asyncio.AbstractEventLoop) -> None:
        self._loop = loop

    def subscribe(self) -> asyncio.Queue[dict[str, Any]]:
        queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue(maxsize=200)
        with self._lock:
            self._clients.add(queue)
        return queue

    def unsubscribe(self, queue: asyncio.Queue[dict[str, Any]]) -> None:
        with self._lock:
            self._clients.discard(queue)

    def publish_threadsafe(self, message: dict[str, Any]) -> None:
        if self._loop is None:
            return

        def publish() -> None:
            with self._lock:
                queues = list(self._clients)
            for queue in queues:
                if queue.full():
                    try:
                        queue.get_nowait()
                    except asyncio.QueueEmpty:
                        pass
                queue.put_nowait(message)

        self._loop.call_soon_threadsafe(publish)
