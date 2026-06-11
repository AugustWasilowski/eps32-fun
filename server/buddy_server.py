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
import asyncio
import audioop
import io
import os
import tempfile
import uuid
import wave
from datetime import datetime
from zoneinfo import ZoneInfo

import httpx
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import JSONResponse, PlainTextResponse, Response

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

# Leo target (a second Claude Code on August's Windows PC). Its channel runs on
# that PC; reply_url must be how Leo's PC reaches THIS server (max's LAN IP).
LEO_CHANNEL_URL = os.environ.get("LEO_CHANNEL_URL", "")
LEO_CHANNEL_TOKEN = os.environ.get("LEO_CHANNEL_TOKEN", "")
LEO_REPLY_URL = os.environ.get("LEO_REPLY_URL", "")

# Air mode = voice notes. Appended as a timestamped running log to this file,
# in the shared inbox both Leo and Max can read.
NOTES_DIR = os.environ.get("NOTES_DIR", "/inbox")
NOTES_FILE = os.path.join(NOTES_DIR, "notes.md")
NOTES_TZ = os.environ.get("NOTES_TZ", "America/Chicago")

# Piper TTS service (on the host) used to speak replies back through the buddy.
PIPER_URL = os.environ.get("PIPER_URL", "http://host.docker.internal:5050")
SPEAK_REPLIES = os.environ.get("SPEAK_REPLIES", "1") == "1"


MODECLIP_CACHE: dict[str, bytes] = {}


async def synth_pcm16(text: str) -> bytes:
    """Piper TTS -> 16 kHz mono 16-bit raw PCM (the buddy's playback format)."""
    async with httpx.AsyncClient(timeout=30) as client:
        r = await client.post(PIPER_URL, json={"text": text[:1000]})
        r.raise_for_status()
        wav_bytes = r.content
    with wave.open(io.BytesIO(wav_bytes), "rb") as w:
        rate, ch = w.getframerate(), w.getnchannels()
        frames = w.readframes(w.getnframes())
    if ch == 2:
        frames = audioop.tomono(frames, 2, 0.5, 0.5)
    pcm16k, _ = audioop.ratecv(frames, 2, 1, rate, 16000, None)
    # Peak-normalize: Piper's output isn't full-scale, so the buddy's small speaker
    # sounds quiet even at codec volume 100. Scale up so the loudest sample reaches
    # ~97% of full scale (3% headroom = no clipping). Cap the boost at 8x so near-
    # silence (e.g. a one-word reply with a long tail) isn't amplified into noise.
    peak = audioop.max(pcm16k, 2)
    if peak > 0:
        factor = min(int(0.97 * 32767) / peak, 8.0)
        if factor > 1.0:
            pcm16k = audioop.mul(pcm16k, 2, factor)
    return pcm16k


async def speak_on_buddy(text: str) -> None:
    """Synthesize `text` with Piper and play it through the buddy's speaker
    (also shows the text on the e-paper via X-Buddy-Text)."""
    ip = STATE.get("buddy_ip")
    if not text or not ip:
        return
    try:
        pcm16k = await synth_pcm16(text)
        hdr_text = text.replace("\n", " ").replace("\r", " ")[:200].encode("ascii", "ignore").decode()
        async with httpx.AsyncClient(timeout=30) as client:
            await client.post(
                f"http://{ip}:{STATE.get('buddy_port', 8080)}/play",
                content=pcm16k,
                headers={
                    "X-Buddy-Token": BUDDY_TOKEN,
                    "X-Buddy-Text": hdr_text,
                    "Content-Type": "application/octet-stream",
                },
            )
    except Exception as exc:  # noqa: BLE001 — speaking is best-effort
        print(f"[buddy] speak_on_buddy failed: {exc}")


def append_note(text: str, epoch: int) -> None:
    """Append a timestamped voice note to the shared notes.md running log."""
    if not text:
        return
    try:
        tz = ZoneInfo(NOTES_TZ)
    except Exception:  # noqa: BLE001
        tz = ZoneInfo("UTC")
    # Use the device's record time when provided (sane epoch), else now.
    when = datetime.fromtimestamp(epoch, tz) if epoch > 1_000_000_000 else datetime.now(tz)
    stamp = when.strftime("%Y-%m-%d %H:%M:%S %Z")
    try:
        os.makedirs(NOTES_DIR, exist_ok=True)
        with open(NOTES_FILE, "a", encoding="utf-8") as f:
            f.write(f"- **{stamp}** — {text}\n")
        print(f"[buddy] note appended ({stamp}): {text}")
    except Exception as exc:  # noqa: BLE001
        print(f"[buddy] note append failed: {exc}")


async def _deliver(text: str, target: str) -> None:
    """Forward a transcript into the chosen assistant's channel."""
    if target == "leo":
        url, token, reply = LEO_CHANNEL_URL, LEO_CHANNEL_TOKEN, LEO_REPLY_URL
    else:
        url, token, reply = SS_CHAT_CHANNEL_URL, SS_CHAT_TOKEN, SELF_REPLY_URL
    if not text or not url or not token:
        if target == "leo":
            print("[buddy] leo target but LEO_CHANNEL_URL/TOKEN not set — not delivered")
        return
    payload = {"request_id": "voice-" + uuid.uuid4().hex[:12], "text": f"[voice] {text}", "host": "max"}
    if reply:
        payload["reply_url"] = reply
    try:
        async with httpx.AsyncClient(timeout=10) as client:
            await client.post(url, json=payload, headers={"X-Webhook-Token": token})
    except Exception as exc:  # noqa: BLE001 — never fail the device on a delivery hiccup
        print(f"[buddy] {target}-channel post failed: {exc}")

app = FastAPI(title="claude-buddy")

# Tiny in-memory state: the most recent transcript, Max's reply, and where to
# reach the buddy's /play server (it registers its DHCP IP on boot / each poll).
STATE = {"last_transcript": "", "max_reply": "", "buddy_ip": "", "buddy_port": 8080}


def _auth(token: str | None) -> None:
    if not token or token != BUDDY_TOKEN:
        raise HTTPException(status_code=403, detail="forbidden")


@app.post("/transcribe")
async def transcribe(
    request: Request,
    x_buddy_token: str | None = Header(default=None),
    x_buddy_target: str | None = Header(default=None),
    x_buddy_time: str | None = Header(default=None),
):
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

    # Route by target: note -> notes.md running log; otherwise into the chosen
    # assistant's channel (max | leo). x_buddy_time = device record epoch (notes).
    target = (x_buddy_target or "max").lower()
    if target == "note":
        try:
            epoch = int(x_buddy_time) if x_buddy_time else 0
        except ValueError:
            epoch = 0
        append_note(text, epoch)
    else:
        await _deliver(text, target)

    return PlainTextResponse(text)


@app.post("/chat_reply")
async def chat_reply(request: Request, x_webhook_token: str | None = Header(default=None)):
    """Receive a reply from a channel (Max's ss_chat_reply or Leo's leo_chat_reply)."""
    valid = {t for t in (SS_CHAT_TOKEN, LEO_CHANNEL_TOKEN) if t}
    if not x_webhook_token or x_webhook_token not in valid:
        raise HTTPException(status_code=403, detail="forbidden")
    body = await request.json()
    text = (body or {}).get("text", "") or ""
    STATE["max_reply"] = text
    # Speak the reply back through the buddy (Piper -> /play). Both Max & Leo
    # replies flow through here, so neither needs a TTS skill of its own.
    if SPEAK_REPLIES and text:
        asyncio.create_task(speak_on_buddy(text))
    return JSONResponse({"ok": True})


@app.post("/register")
async def register(request: Request, x_buddy_token: str | None = Header(default=None)):
    """The buddy reports its (DHCP) IP + /play port so TTS senders know where to go."""
    _auth(x_buddy_token)
    body = await request.json()
    ip = (body or {}).get("ip", "")
    if not ip:
        raise HTTPException(status_code=400, detail="ip required")
    STATE["buddy_ip"] = ip
    STATE["buddy_port"] = int((body or {}).get("port", 8080))
    return JSONResponse({"ok": True})


@app.get("/buddy")
async def buddy(x_buddy_token: str | None = Header(default=None)):
    """Where to reach the buddy's /play endpoint (for Piper TTS senders on max)."""
    _auth(x_buddy_token)
    ip, port = STATE["buddy_ip"], STATE["buddy_port"]
    return JSONResponse(
        {"ip": ip, "port": port, "play_url": f"http://{ip}:{port}/play" if ip else ""}
    )


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


@app.get("/modeclip")
async def modeclip(word: str = "", x_buddy_token: str | None = Header(default=None)):
    """Return a short spoken word ("Max"/"Leo"/"Notes") as 16 kHz mono 16-bit raw
    PCM, for the buddy to cache on SD and play on mode switches. Cached per word."""
    _auth(x_buddy_token)
    w = (word or "").strip()
    if not w:
        raise HTTPException(status_code=400, detail="word required")
    if w not in MODECLIP_CACHE:
        MODECLIP_CACHE[w] = await synth_pcm16(w)
    return Response(content=MODECLIP_CACHE[w], media_type="application/octet-stream")


@app.get("/healthz")
async def healthz():
    return {"ok": True}
