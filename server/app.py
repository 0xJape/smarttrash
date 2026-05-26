"""
Smart Trash — Jetson API server.

Receives sensor readings from the ESP32 over HTTP and serves a small
dashboard. Designed to run behind a Cloudflare named tunnel.

Run locally:
    SMARTRASH_TOKEN=dev-token uvicorn app:app --host 0.0.0.0 --port 8000

Production (systemd unit ships in deploy/smartrash-api.service).
"""

from __future__ import annotations

import os
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Optional

from fastapi import Depends, FastAPI, Header, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel, Field

BASE_DIR = Path(__file__).resolve().parent
DB_PATH = Path(os.environ.get("SMARTRASH_DB", BASE_DIR / "smartrash.db"))
DEVICE_TOKEN = os.environ.get("SMARTRASH_TOKEN", "change-me")

# Distance (cm) at or below which the bin is considered full.
FULL_THRESHOLD_CM = float(os.environ.get("SMARTRASH_FULL_CM", "10"))
# Distance (cm) used as "empty" reference for the percentage calc.
EMPTY_DISTANCE_CM = float(os.environ.get("SMARTRASH_EMPTY_CM", "40"))

app = FastAPI(title="Smart Trash API", version="1.0.0")
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))


# --------------------------------------------------------------------------- #
# Database
# --------------------------------------------------------------------------- #

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id                 INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id          TEXT    NOT NULL,
    received_at        REAL    NOT NULL,
    distance_lid_cm    REAL,
    distance_level_cm  REAL,
    sound              INTEGER NOT NULL,
    lid_open           INTEGER NOT NULL,
    is_full            INTEGER NOT NULL,
    lid_open_count     INTEGER,
    sound_event_count  INTEGER,
    uptime_ms          INTEGER,
    rssi               INTEGER
);
CREATE INDEX IF NOT EXISTS idx_readings_device_time
    ON readings(device_id, received_at DESC);

CREATE TABLE IF NOT EXISTS commands (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id   TEXT    NOT NULL,
    action      TEXT    NOT NULL,
    created_at  REAL    NOT NULL,
    delivered_at REAL,
    acked_at    REAL
);
CREATE INDEX IF NOT EXISTS idx_commands_pending
    ON commands(device_id, delivered_at);

CREATE TABLE IF NOT EXISTS device_state (
    device_id   TEXT PRIMARY KEY,
    alarm_mode  INTEGER NOT NULL DEFAULT 0,
    lid_held    INTEGER NOT NULL DEFAULT 0,
    updated_at  REAL    NOT NULL
);
"""


@contextmanager
def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    try:
        yield conn
        conn.commit()
    finally:
        conn.close()


def init_db() -> None:
    with db() as conn:
        conn.executescript(SCHEMA)


init_db()


# --------------------------------------------------------------------------- #
# Auth
# --------------------------------------------------------------------------- #

def require_device_token(x_device_token: Optional[str] = Header(default=None)) -> None:
    if not x_device_token or x_device_token != DEVICE_TOKEN:
        raise HTTPException(status_code=401, detail="invalid device token")


# --------------------------------------------------------------------------- #
# Schemas
# --------------------------------------------------------------------------- #

class Reading(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    distance_lid_cm: float
    distance_level_cm: float
    sound: bool
    lid_open: bool
    is_full: bool
    lid_open_count: int = 0
    sound_event_count: int = 0
    uptime_ms: int = 0
    rssi: int = 0


# --------------------------------------------------------------------------- #
# Routes
# --------------------------------------------------------------------------- #

@app.get("/health")
def health() -> dict:
    return {"ok": True, "ts": time.time()}


@app.post("/ingest", dependencies=[Depends(require_device_token)])
def ingest(reading: Reading) -> dict:
    now = time.time()
    with db() as conn:
        conn.execute(
            """
            INSERT INTO readings (
                device_id, received_at,
                distance_lid_cm, distance_level_cm,
                sound, lid_open, is_full,
                lid_open_count, sound_event_count,
                uptime_ms, rssi
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                reading.device_id, now,
                reading.distance_lid_cm, reading.distance_level_cm,
                int(reading.sound), int(reading.lid_open), int(reading.is_full),
                reading.lid_open_count, reading.sound_event_count,
                reading.uptime_ms, reading.rssi,
            ),
        )
    return {"ok": True, "received_at": now}


def _level_percent(distance_level_cm: Optional[float]) -> Optional[int]:
    if distance_level_cm is None or distance_level_cm < 0:
        return None
    span = max(EMPTY_DISTANCE_CM - FULL_THRESHOLD_CM, 1.0)
    pct = (EMPTY_DISTANCE_CM - distance_level_cm) / span * 100.0
    return max(0, min(100, int(round(pct))))


def _latest_per_device() -> list[dict]:
    with db() as conn:
        rows = conn.execute(
            """
            SELECT r.*
            FROM readings r
            JOIN (
                SELECT device_id, MAX(received_at) AS max_ts
                FROM readings GROUP BY device_id
            ) latest
              ON latest.device_id = r.device_id
             AND latest.max_ts    = r.received_at
            ORDER BY r.device_id
            """
        ).fetchall()
    out = []
    for r in rows:
        d = dict(r)
        d["level_percent"] = _level_percent(d.get("distance_level_cm"))
        d["age_seconds"] = round(time.time() - d["received_at"], 1)
        out.append(d)
    return out


@app.get("/status")
def status() -> JSONResponse:
    return JSONResponse({"devices": _latest_per_device()})


@app.get("/history")
def history(device_id: str, limit: int = 100) -> JSONResponse:
    limit = max(1, min(limit, 1000))
    with db() as conn:
        rows = conn.execute(
            """
            SELECT received_at, distance_level_cm, distance_lid_cm,
                   sound, lid_open, is_full, rssi
              FROM readings
             WHERE device_id = ?
             ORDER BY received_at DESC
             LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
    return JSONResponse({"device_id": device_id, "readings": [dict(r) for r in rows]})


@app.get("/", response_class=HTMLResponse)
def dashboard(request: Request) -> HTMLResponse:
    return templates.TemplateResponse(
        "dashboard.html",
        {"request": request, "full_cm": FULL_THRESHOLD_CM, "empty_cm": EMPTY_DISTANCE_CM},
    )


# --------------------------------------------------------------------------- #
# Commands (remote control)
# --------------------------------------------------------------------------- #

# Actions the ESP32 knows how to execute. Keep this in sync with the firmware.
VALID_ACTIONS = {"open_lid", "close_lid", "beep"}


class CommandIn(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    action: str


@app.post("/command")
def queue_command(cmd: CommandIn) -> dict:
    if cmd.action not in VALID_ACTIONS:
        raise HTTPException(status_code=400, detail=f"unknown action: {cmd.action}")
    now = time.time()
    with db() as conn:
        if cmd.action in ("open_lid", "close_lid"):
            conn.execute(
                """
                INSERT INTO device_state (device_id, lid_held, updated_at)
                VALUES (?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    lid_held   = excluded.lid_held,
                    updated_at = excluded.updated_at
                """,
                (cmd.device_id, 1 if cmd.action == "open_lid" else 0, now),
            )
        cur = conn.execute(
            "INSERT INTO commands (device_id, action, created_at) VALUES (?, ?, ?)",
            (cmd.device_id, cmd.action, now),
        )
        cmd_id = cur.lastrowid
    return {"ok": True, "command_id": cmd_id}


@app.get("/commands", dependencies=[Depends(require_device_token)])
def fetch_commands(device_id: str) -> JSONResponse:
    """Called by the ESP32. Returns at most one pending command and marks it
    as delivered so it isn't replayed on the next poll."""
    now = time.time()
    with db() as conn:
        row = conn.execute(
            """
            SELECT id, action FROM commands
             WHERE device_id = ? AND delivered_at IS NULL
             ORDER BY id ASC LIMIT 1
            """,
            (device_id,),
        ).fetchone()
        if not row:
            return JSONResponse({"command": None})
        conn.execute(
            "UPDATE commands SET delivered_at = ? WHERE id = ?",
            (now, row["id"]),
        )
    return JSONResponse({"command": {"id": row["id"], "action": row["action"]}})


class AckIn(BaseModel):
    command_id: int


@app.post("/commands/ack", dependencies=[Depends(require_device_token)])
def ack_command(ack: AckIn) -> dict:
    with db() as conn:
        conn.execute(
            "UPDATE commands SET acked_at = ? WHERE id = ?",
            (time.time(), ack.command_id),
        )
    return {"ok": True}


@app.get("/state")
def get_state(device_id: str) -> JSONResponse:
    """Desired control state for the dashboard (lid held open)."""
    with db() as conn:
        row = conn.execute(
            "SELECT lid_held FROM device_state WHERE device_id = ?",
            (device_id,),
        ).fetchone()
    if not row:
        return JSONResponse({"lid_held": False})
    return JSONResponse({"lid_held": bool(row["lid_held"])})
