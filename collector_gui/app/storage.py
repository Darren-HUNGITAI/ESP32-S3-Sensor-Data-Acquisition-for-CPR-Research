from __future__ import annotations

import json
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class Storage:
    def __init__(self, sqlite_path: str) -> None:
        self.sqlite_path = sqlite_path
        Path(sqlite_path).parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._connection = sqlite3.connect(
            sqlite_path,
            check_same_thread=False,
            isolation_level=None,
        )
        self._connection.row_factory = sqlite3.Row
        self.initialize()

    def initialize(self) -> None:
        with self._lock:
            self._connection.executescript(
                """
                PRAGMA journal_mode = WAL;
                PRAGMA synchronous = NORMAL;

                CREATE TABLE IF NOT EXISTS imu_samples (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    received_at TEXT NOT NULL,
                    topic TEXT NOT NULL,
                    uptime_ms INTEGER,
                    acc_x_mg REAL,
                    acc_y_mg REAL,
                    acc_z_mg REAL,
                    gyr_x_dps REAL,
                    gyr_y_dps REAL,
                    gyr_z_dps REAL,
                    mag_x_uT REAL,
                    mag_y_uT REAL,
                    mag_z_uT REAL,
                    temp_c REAL,
                    raw_json TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_imu_samples_received_at
                    ON imu_samples(received_at);

                CREATE TABLE IF NOT EXISTS device_status (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    received_at TEXT NOT NULL,
                    topic TEXT NOT NULL,
                    status TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_device_status_received_at
                    ON device_status(received_at);

                CREATE TABLE IF NOT EXISTS collector_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    received_at TEXT NOT NULL,
                    topic TEXT,
                    level TEXT NOT NULL,
                    message TEXT NOT NULL,
                    raw_payload TEXT
                );
                """
            )

    @staticmethod
    def _nested_float(payload: dict[str, Any], outer: str, inner: str) -> float | None:
        value = payload.get(outer)
        if not isinstance(value, dict):
            return None
        nested = value.get(inner)
        if nested is None:
            return None
        try:
            return float(nested)
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _optional_int(value: Any) -> int | None:
        if value is None:
            return None
        try:
            return int(value)
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _optional_float(value: Any) -> float | None:
        if value is None:
            return None
        try:
            return float(value)
        except (TypeError, ValueError):
            return None

    def insert_sample(self, topic: str, payload: dict[str, Any], raw_json: str) -> dict[str, Any]:
        received_at = utc_now_iso()
        values = {
            "received_at": received_at,
            "topic": topic,
            "uptime_ms": self._optional_int(payload.get("uptime_ms")),
            "acc_x_mg": self._nested_float(payload, "acc_mg", "x"),
            "acc_y_mg": self._nested_float(payload, "acc_mg", "y"),
            "acc_z_mg": self._nested_float(payload, "acc_mg", "z"),
            "gyr_x_dps": self._nested_float(payload, "gyr_dps", "x"),
            "gyr_y_dps": self._nested_float(payload, "gyr_dps", "y"),
            "gyr_z_dps": self._nested_float(payload, "gyr_dps", "z"),
            "mag_x_uT": self._nested_float(payload, "mag_uT", "x"),
            "mag_y_uT": self._nested_float(payload, "mag_uT", "y"),
            "mag_z_uT": self._nested_float(payload, "mag_uT", "z"),
            "temp_c": self._optional_float(payload.get("temp_c")),
            "raw_json": raw_json,
        }

        with self._lock:
            cursor = self._connection.execute(
                """
                INSERT INTO imu_samples (
                    received_at, topic, uptime_ms,
                    acc_x_mg, acc_y_mg, acc_z_mg,
                    gyr_x_dps, gyr_y_dps, gyr_z_dps,
                    mag_x_uT, mag_y_uT, mag_z_uT,
                    temp_c, raw_json
                ) VALUES (
                    :received_at, :topic, :uptime_ms,
                    :acc_x_mg, :acc_y_mg, :acc_z_mg,
                    :gyr_x_dps, :gyr_y_dps, :gyr_z_dps,
                    :mag_x_uT, :mag_y_uT, :mag_z_uT,
                    :temp_c, :raw_json
                )
                """,
                values,
            )
            sample_id = int(cursor.lastrowid)

        return {"id": sample_id, **values}

    def insert_status(self, topic: str, status: str) -> dict[str, Any]:
        received_at = utc_now_iso()
        clean_status = status.strip() or "unknown"
        with self._lock:
            cursor = self._connection.execute(
                """
                INSERT INTO device_status (received_at, topic, status)
                VALUES (?, ?, ?)
                """,
                (received_at, topic, clean_status),
            )
            status_id = int(cursor.lastrowid)
        return {
            "id": status_id,
            "received_at": received_at,
            "topic": topic,
            "status": clean_status,
        }

    def insert_event(
        self,
        level: str,
        message: str,
        topic: str | None = None,
        raw_payload: str | None = None,
    ) -> dict[str, Any]:
        received_at = utc_now_iso()
        with self._lock:
            cursor = self._connection.execute(
                """
                INSERT INTO collector_events (received_at, topic, level, message, raw_payload)
                VALUES (?, ?, ?, ?, ?)
                """,
                (received_at, topic, level, message, raw_payload),
            )
            event_id = int(cursor.lastrowid)
        return {
            "id": event_id,
            "received_at": received_at,
            "topic": topic,
            "level": level,
            "message": message,
            "raw_payload": raw_payload,
        }

    def list_samples(self, limit: int = 500) -> list[dict[str, Any]]:
        safe_limit = max(1, min(limit, 5000))
        with self._lock:
            rows = self._connection.execute(
                """
                SELECT *
                FROM imu_samples
                ORDER BY id DESC
                LIMIT ?
                """,
                (safe_limit,),
            ).fetchall()
        return [dict(row) for row in reversed(rows)]

    def latest_status(self) -> dict[str, Any] | None:
        with self._lock:
            row = self._connection.execute(
                """
                SELECT *
                FROM device_status
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()
        return dict(row) if row else None

    def summary(self) -> dict[str, Any]:
        with self._lock:
            count_row = self._connection.execute(
                "SELECT COUNT(*) AS total_samples FROM imu_samples"
            ).fetchone()
            latest_sample = self._connection.execute(
                """
                SELECT *
                FROM imu_samples
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()
            latest_status = self._connection.execute(
                """
                SELECT *
                FROM device_status
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()
            latest_event = self._connection.execute(
                """
                SELECT *
                FROM collector_events
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()

        return {
            "total_samples": int(count_row["total_samples"]),
            "latest_sample": dict(latest_sample) if latest_sample else None,
            "latest_status": dict(latest_status) if latest_status else None,
            "latest_event": dict(latest_event) if latest_event else None,
        }

    def decode_sample_payload(self, topic: str, payload_text: str) -> dict[str, Any] | None:
        try:
            decoded = json.loads(payload_text)
        except json.JSONDecodeError as exc:
            self.insert_event(
                "warning",
                f"Malformed JSON ignored: {exc.msg}",
                topic=topic,
                raw_payload=payload_text,
            )
            return None

        if not isinstance(decoded, dict):
            self.insert_event(
                "warning",
                "JSON payload was not an object",
                topic=topic,
                raw_payload=payload_text,
            )
            return None

        return decoded
