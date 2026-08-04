"""
gateway/main.py — Smart Surveillance System, Step 4 (REST API + Swagger)

THIN LAYER ONLY. Every endpoint here is a straight proxy to the C server
(see ../src/api_router.c) — this file adds request/response typing for
Swagger UI and nothing else. No business logic, no data transformation,
no state. If you're tempted to add a computation, validation rule, or
anything beyond "forward the request, forward the response" here, it
belongs in the C server instead — that's the assignment's actual
constraint ("Core logic in C; FastAPI is allowed only as a thin
documentation/gateway layer on top").

Run:
    pip install -r requirements.txt
    uvicorn main:app --host 0.0.0.0 --port 8000

Then open http://<this-host>:8000/docs for Swagger UI.

Configuration (environment variables, all optional):
    C_SERVER_HOST   default "127.0.0.1" — where the C HTTPS server runs
    C_SERVER_PORT   default 443         — its https_port from server.conf
    C_SERVER_VERIFY_TLS  default "false" — the C server uses the
        self-signed cert from Step 1 (secure_setup.sh), so TLS
        verification is off by default. Set to "true" if you've since
        installed a CA-trusted cert.
"""
import os
from typing import List

import httpx
from fastapi import FastAPI, HTTPException, Response
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

C_SERVER_HOST = os.environ.get("C_SERVER_HOST", "127.0.0.1")
C_SERVER_PORT = int(os.environ.get("C_SERVER_PORT", "443"))
C_SERVER_VERIFY_TLS = os.environ.get("C_SERVER_VERIFY_TLS", "false").lower() == "true"
C_SERVER_BASE = f"https://{C_SERVER_HOST}:{C_SERVER_PORT}"

app = FastAPI(
    title="Smart Surveillance System — REST API",
    description=(
        "Swagger/documentation gateway. Every endpoint below is a thin "
        "proxy to the C HTTPS server — all detection, telemetry, and "
        "command logic actually runs in C (see src/api_router.c, "
        "src/sysinfo.c, src/command_dispatch.c, src/history_log.c). "
        "This gateway exists purely to give the API typed schemas and "
        "an interactive Swagger UI."
    ),
    version="1.0.0",
)


# --- Pydantic schemas (docs/typing only — the C server is the source of truth) ---

class PersonsResponse(BaseModel):
    count: int = Field(..., description="Number of people in the current frame")
    timestamp: str = Field(..., description="Timestamp of the detection frame this count came from")


class TelemetryResponse(BaseModel):
    cpu_temp_c: float = Field(..., description="SoC temperature in Celsius, read from /sys/class/thermal")
    cpu_percent: float = Field(..., description="CPU utilization percentage, computed from /proc/stat deltas")
    mem_free_mb: float = Field(..., description="Available memory in MB, from /proc/meminfo")
    mem_total_mb: float = Field(..., description="Total memory in MB, from /proc/meminfo")
    uptime_seconds: float = Field(..., description="Seconds since boot, from /proc/uptime")
    load_avg_1m: float = Field(..., description="1-minute load average, from /proc/loadavg")
    load_avg_5m: float = Field(..., description="5-minute load average, from /proc/loadavg")
    load_avg_15m: float = Field(..., description="15-minute load average, from /proc/loadavg")
    disk_free_mb: float = Field(..., description="Free disk space in MB for the root filesystem, via statvfs()")
    disk_total_mb: float = Field(..., description="Total disk space in MB for the root filesystem, via statvfs()")
    timestamp: str = Field(..., description="UTC timestamp this sample was taken")


class CommandRequest(BaseModel):
    cmd: str = Field(..., description="Command name", examples=["noop", "reboot", "shutdown"])


class CommandResponse(BaseModel):
    cmd: str
    status: str = Field(..., description="'ok', 'refused', or 'unknown'")
    message: str


class HistoryRecord(BaseModel):
    count: int
    timestamp: str


class HistorySummaryResponse(BaseModel):
    records_stored: int = Field(..., description="How many of the 5 ring-buffer slots are currently filled")
    capacity: int = Field(..., description="Ring buffer size (fixed at 5)")
    min_count: int
    max_count: int
    avg_count: float


class ServiceStatus(BaseModel):
    name: str
    status: str = Field(..., description="systemd ActiveState, e.g. 'active', 'inactive', 'failed'")


class ServicesListResponse(BaseModel):
    services: List[ServiceStatus]


class ServiceRestartResponse(BaseModel):
    service: str
    status: str = Field(..., description="'ok' or 'error'")
    message: str


class ServiceLogsResponse(BaseModel):
    service: str
    lines_requested: int
    logs: str = Field(..., description="Raw journalctl output, newline-separated")


def _client() -> httpx.Client:
    return httpx.Client(base_url=C_SERVER_BASE, verify=C_SERVER_VERIFY_TLS, timeout=10.0)


def _proxy_get(path: str) -> httpx.Response:
    try:
        with _client() as client:
            resp = client.get(path)
        return resp
    except httpx.ConnectError as exc:
        raise HTTPException(
            status_code=502,
            detail=f"Could not reach C server at {C_SERVER_BASE}{path}: {exc}",
        )


@app.get("/api/v1/persons", response_model=PersonsResponse, tags=["persons"],
         summary="Current person count")
def get_persons():
    resp = _proxy_get("/api/v1/persons")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.get("/api/v1/telemetry", response_model=TelemetryResponse, tags=["telemetry"],
         summary="CPU temperature, memory, CPU load")
def get_telemetry():
    resp = _proxy_get("/api/v1/telemetry")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.get("/api/v1/history", response_model=List[HistoryRecord], tags=["history"],
         summary="Last up to 5 detection records")
def get_history():
    resp = _proxy_get("/api/v1/history")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.get("/api/v1/history/summary", response_model=HistorySummaryResponse, tags=["history"],
         summary="Min/max/avg person count across stored history")
def get_history_summary():
    resp = _proxy_get("/api/v1/history/summary")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.get("/api/v1/services", response_model=ServicesListResponse, tags=["services"],
         summary="List managed systemd services and their status")
def get_services():
    resp = _proxy_get("/api/v1/services")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.post("/api/v1/services/{name}/restart", response_model=ServiceRestartResponse, tags=["services"],
          summary="Restart a managed service",
          description=(
              "`name` must be one of the allowlisted units the C server "
              "recognizes (see src/service_ctl.c's SERVICE_ALLOWLIST) — "
              "anything else is rejected with 404, no process spawned. "
              "Restarting 'surveillance-web' (the server handling this "
              "very request) is scheduled ~1s in the future instead of "
              "run synchronously, so the response can reach you first; "
              "its result isn't confirmed the way other services' is."
          ))
def post_service_restart(name: str):
    try:
        with _client() as client:
            resp = client.post(f"/api/v1/services/{name}/restart")
    except httpx.ConnectError as exc:
        raise HTTPException(status_code=502, detail=f"Could not reach C server: {exc}")
    return Response(content=resp.content, status_code=resp.status_code, media_type="application/json")


@app.get("/api/v1/services/{name}/logs", response_model=ServiceLogsResponse, tags=["services"],
         summary="Latest journalctl entries for a managed service")
def get_service_logs(name: str, lines: int = 50):
    resp = _proxy_get(f"/api/v1/services/{name}/logs?lines={lines}")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return resp.json()


@app.post("/api/v1/command", response_model=CommandResponse, tags=["command"],
          summary="Send a command to the board",
          description=(
              "Forwards {\"cmd\": \"...\"} to the C server's command "
              "dispatch table (src/command_dispatch.c). 'reboot' and "
              "'shutdown' are refused (HTTP 403) unless "
              "allow_dangerous_commands=true is set in server.conf — "
              "use 'noop' to test the pipeline safely."
          ))
def post_command(body: CommandRequest):
    try:
        with _client() as client:
            resp = client.post("/api/v1/command", json=body.model_dump())
    except httpx.ConnectError as exc:
        raise HTTPException(status_code=502, detail=f"Could not reach C server: {exc}")

    # Forward the C server's status code as-is (200 ok / 403 refused / 404 unknown)
    # instead of collapsing everything to 200, so the real outcome is visible
    # in Swagger UI and to any client checking status codes.
    return Response(
        content=resp.content,
        status_code=resp.status_code,
        media_type="application/json",
    )


@app.get("/api/v1/frame.jpg", tags=["stream"],
         summary="Single still JPEG frame",
         description=(
             "One-shot JPEG, not a video stream — easy to curl, screenshot, "
             "or view directly. Swagger UI's 'Try it out' can render this "
             "one (unlike /api/v1/stream)."
         ))
def get_frame():
    resp = _proxy_get("/api/v1/frame.jpg")
    if resp.status_code != 200:
        raise HTTPException(status_code=resp.status_code, detail=resp.text)
    return Response(content=resp.content, media_type="image/jpeg")


@app.get("/api/v1/stream", tags=["stream"],
         summary="Live MJPEG video",
         description=(
             "Proxies the MJPEG multipart stream from the C server. "
             "Open this URL directly in a browser (Swagger's 'Try it out' "
             "won't render video) — e.g. http://<gateway-host>:8000/api/v1/stream"
         ))
def get_stream():
    def iter_upstream():
        with httpx.stream("GET", f"{C_SERVER_BASE}/api/v1/stream",
                           verify=C_SERVER_VERIFY_TLS, timeout=None) as upstream:
            for chunk in upstream.iter_raw():
                yield chunk

    # The C server's multipart boundary is fixed (see mjpeg_stream.c's
    # BOUNDARY macro) — mirror it here so browsers render the stream
    # exactly as they would talking to the C server directly.
    return StreamingResponse(
        iter_upstream(),
        media_type="multipart/x-mixed-replace; boundary=surveillanceframe",
    )


@app.get("/", tags=["meta"], summary="Gateway health check")
def root():
    return {
        "gateway": "ok",
        "c_server_target": C_SERVER_BASE,
        "docs": "/docs",
    }
