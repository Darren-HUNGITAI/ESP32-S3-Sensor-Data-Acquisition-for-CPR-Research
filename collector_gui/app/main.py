from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Query, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .config import settings
from .mqtt_collector import LiveHub, MqttCollector
from .storage import Storage

BASE_DIR = Path(__file__).resolve().parent
STATIC_DIR = BASE_DIR / "static"

storage = Storage(settings.sqlite_path)
live_hub = LiveHub()
collector = MqttCollector(storage, live_hub.publish_threadsafe)


@asynccontextmanager
async def lifespan(app: FastAPI):
    del app
    live_hub.bind_loop(asyncio.get_running_loop())
    collector.start()
    try:
        yield
    finally:
        collector.stop()


app = FastAPI(title="ICM-20948 Collector GUI", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
def index() -> FileResponse:
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/health")
def health() -> dict[str, Any]:
    return {
        "ok": True,
        "mqtt": collector.snapshot(),
        "sqlite_path": settings.sqlite_path,
    }


@app.get("/api/samples")
def samples(limit: int = Query(default=500, ge=1, le=5000)) -> dict[str, Any]:
    rows = storage.list_samples(limit=limit)
    return {"samples": rows, "count": len(rows)}


@app.get("/api/status")
def status() -> dict[str, Any]:
    return {
        "mqtt": collector.snapshot(),
        "device": storage.latest_status(),
    }


@app.get("/api/summary")
def summary() -> dict[str, Any]:
    return {
        **storage.summary(),
        "mqtt": collector.snapshot(),
    }


@app.websocket("/ws/live")
async def live(websocket: WebSocket) -> None:
    await websocket.accept()
    queue = live_hub.subscribe()
    try:
        await websocket.send_json({"type": "hello", "summary": summary()})
        while True:
            message = await queue.get()
            await websocket.send_json(message)
    except WebSocketDisconnect:
        pass
    finally:
        live_hub.unsubscribe(queue)
