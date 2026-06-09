"""buddy_server — max-side FastAPI service for the ESP32 Claude Buddy.

Gated by the X-Buddy-Token header (must match BUDDY_TOKEN):

  POST /transcribe   body = raw WAV bytes (audio/wav)
                     -> faster-whisper -> text
                     -> forwarded to the WORKING ss-chat-channel (:8802) so it
                        surfaces as a turn in Max's always-on ss-channels session
                     -> returns the transcript as text/plain (device "last transcript")

  POST /chat_reply   {request_id, text}  — called by ss-chat-channel when Max replies
                     to a voice turn (via its ss_chat_reply tool). Gated by
                     X-Webhook-Token == SS_CHAT_TOKEN. Stored for /display.

  GET  /display      -> {"tokens", "cost", "context_pct", "last_transcript", "max_reply"}

Delivery note: the original voice-channel emitted a notifications/claude/channel
event but it never surfaced (one-way / no reply tool). ss-chat-channel is the
proven two-way path (Max replies via ss_chat_reply), so we route through it.

Run (dev):   uvicorn buddy_server:app --host 0.0.0.0 --port 8810
Run (prod):  see docker-compose.yml
"""
import os
import tempfile
import uuid

import httpx
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse, PlainTextResponse

from usage import context_pct, token_usage_today
from whisper import transcribe_wav

BUDDY_TOKEN = os.environ["BUDDY_TOKEN"]

# Deliver transcripts into Max's session via the working ss-chat-channel. From the
# container we reach the host-side channel via host.docker.internal; the channel's
# X-Webhook-Token (SS_CHAT_TOKEN) is the security boundary.
SS_CHAT_CHANNEL_URL = os.environ.get("SS_CHAT_CHANNEL_URL", "http://host.docker.internal:8802/")
SS_CHAT_TOKEN = os.environ.get("SS_CHAT_TOKEN", "")
# Where ss-chat-channel sends Max's reply. Must be loopback for the channel to
# honor the per-request override; our container's 8810 is published on the host,
# so localhost:8810 reaches us from the host where the channel runs.
SELF_REPLY_URL = os.environ.get("SELF_REPLY_URL", "http://localhost:8810/chat_reply")

app = FastAPI(title="claude-buddy")

# Tiny in-memory state: the most recent transcript and Max's most recent reply.
STATE = {"last_transcript": "", "max_reply": ""}


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

    # Surface it in Max's session via ss-chat-channel (request_id correlates Max's
    # reply, which routes back to /chat_reply via reply_url).
    if text and SS_CHAT_TOKEN:
        request_id = "voice-" + uuid.uuid4().hex[:12]
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                await client.post(
                    SS_CHAT_CHANNEL_URL,
                    json={
                        "request_id": request_id,
                        "text": f"[voice] {text}",
                        "host": "max",
                        "reply_url": SELF_REPLY_URL,
                    },
                    headers={"X-Webhook-Token": SS_CHAT_TOKEN},
                )
        except Exception as exc:  # noqa: BLE001 — never fail the device on a delivery hiccup
            print(f"[buddy] ss-chat-channel post failed: {exc}")

    return PlainTextResponse(text)


@app.post("/chat_reply")
async def chat_reply(request: Request, x_webhook_token: str | None = Header(default=None)):
    """Receive Max's reply from ss-chat-channel (its ss_chat_reply -> reply_url)."""
    if not x_webhook_token or x_webhook_token != SS_CHAT_TOKEN:
        raise HTTPException(status_code=403, detail="forbidden")
    body = await request.json()
    STATE["max_reply"] = (body or {}).get("text", "") or ""
    return JSONResponse({"ok": True})


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
            "max_reply": STATE["max_reply"],
        }
    )


@app.get("/healthz")
async def healthz():
    return {"ok": True}
