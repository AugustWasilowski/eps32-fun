"""buddy_server — max-side FastAPI service for the ESP32 Claude Buddy.

Two endpoints, both gated by the X-Buddy-Token header (must match BUDDY_TOKEN):

  POST /transcribe   body = raw WAV bytes (audio/wav)
                     -> faster-whisper -> text
                     -> forwarded to the voice-channel (so it surfaces in Max's session)
                     -> returns the transcript as text/plain (device shows it as "last transcript")

  GET  /display      -> {"tokens": int|None, "cost": float|None,
                         "context_pct": float|None, "last_transcript": str}

Run (dev):   uvicorn buddy_server:app --host 0.0.0.0 --port 8810
Run (prod):  see docker-compose.yml
"""
import os
import tempfile

import httpx
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse, PlainTextResponse

from usage import context_pct, token_usage_today
from whisper import transcribe_wav

BUDDY_TOKEN = os.environ["BUDDY_TOKEN"]
# The voice-channel HTTP endpoint (voice-channel/). When this server runs in a
# container, the channel runs on the host -> use host.docker.internal (mapped in
# docker-compose.yml). The X-Webhook-Token gate is the security boundary.
VOICE_CHANNEL_URL = os.environ.get("VOICE_CHANNEL_URL", "http://host.docker.internal:8803/")
VOICE_CHANNEL_TOKEN = os.environ.get("VOICE_CHANNEL_TOKEN", "")

app = FastAPI(title="claude-buddy")

# Tiny in-memory state; the only thing we keep is the most recent transcript.
STATE = {"last_transcript": ""}


def _auth(token: str | None) -> None:
    if not token or token != BUDDY_TOKEN:
        raise HTTPException(status_code=403, detail="forbidden")


@app.post("/transcribe")
async def transcribe(request: Request, x_buddy_token: str | None = Header(default=None)):
    _auth(x_buddy_token)

    audio = await request.body()
    if not audio:
        raise HTTPException(status_code=400, detail="empty body")

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        f.write(audio)
        path = f.name
    try:
        text = transcribe_wav(path)
    finally:
        os.unlink(path)

    STATE["last_transcript"] = text

    # Inject into Max's always-on session via the voice-channel.
    if text and VOICE_CHANNEL_TOKEN:
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                await client.post(
                    VOICE_CHANNEL_URL,
                    json={"text": text, "source": "voice"},
                    headers={"X-Webhook-Token": VOICE_CHANNEL_TOKEN},
                )
        except Exception as exc:  # noqa: BLE001 — never fail the device on a channel hiccup
            app.logger.warning("voice-channel post failed: %s", exc) if hasattr(app, "logger") else print(
                f"[buddy] voice-channel post failed: {exc}"
            )

    return PlainTextResponse(text)


@app.get("/display")
async def display(x_buddy_token: str | None = Header(default=None)):
    _auth(x_buddy_token)
    usage = token_usage_today()  # {"tokens": int|None, "cost": float|None}
    return JSONResponse(
        {
            "tokens": usage.get("tokens"),
            "cost": usage.get("cost"),
            "context_pct": context_pct(),
            "last_transcript": STATE["last_transcript"],
        }
    )


@app.get("/healthz")
async def healthz():
    return {"ok": True}
