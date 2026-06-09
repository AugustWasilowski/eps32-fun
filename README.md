# eps32-fun — ESP32-S3 e-Paper "Claude Buddy"

A desktop companion built on a **Waveshare ESP32-S3-ePaper-1.54** board that:

1. **Shows** Claude **token usage**, the always-on **Max session's context-window %**, and the
   **last thing you said to it** on the 1.54" e-paper.
2. Has a **push-to-talk mic** (ES8311 codec) — hold the button, speak, release; the audio is sent
   to **max**, transcribed with self-hosted **Whisper**, and the text is injected as a turn into
   the always-on **`ss-channels`** Claude Code session ("Max"). Talking to the buddy = talking to Max.

```
  ESP32-S3-ePaper-1.54  (firmware/buddy, ESP-IDF)
   ├─ hold button → record mic (ES8311 → I2S 16kHz) → POST /transcribe ──┐
   └─ poll GET /display → render e-paper                                 │  WiFi
              ▲                                                           ▼
              │                       ┌────────────────────────────────────────┐
              │   {tokens, cost,      │  max:  server/  (FastAPI + faster-whisper)│
              └───  ctx_pct,  ────────┤  /transcribe → Whisper → voice-channel   │
                    last_transcript}  │  /display   → usage + ctx% + last text   │
                                      └───────────────┬──────────────────────────┘
                                                      │ MCP notification
                                                      ▼
                                        ss-channels tmux session  ("Max")
```

## Layout

| Path | Runs on | What it is |
| --- | --- | --- |
| [`firmware/buddy/`](firmware/buddy/) | The board | ESP-IDF project. Derived from Waveshare's `08_Audio_Test` (ES8311 mic + e-paper + button + LVGL) with the `esp_wifi_bsp` WiFi component added. Currently the stock audio+e-paper demo; the buddy app is built on top. |
| [`firmware/waveshare-examples/`](firmware/waveshare-examples/) | — | Unmodified Waveshare reference examples kept for wiring reference (`06_WIFI_STA`). |
| [`server/`](server/) | max | FastAPI service: `POST /transcribe` (Whisper) and `GET /display` (usage + context %). Dockerized. |
| [`voice-channel/`](voice-channel/) | max | Claude Code custom channel plugin — injects each transcript as a turn into the `ss-channels` session. Mirrors the working `vikunja-channel`. |

## Hardware notes (this board)

- **Flash port:** `COM4` (native USB-CDC, VID `303A` / PID `1001` — no driver needed).
- **WiFi MAC:** `70:04:1D:DB:DD:6C`.
- **ESP-IDF:** v5.5.0+ required. Develop in VS Code with the ESP-IDF extension.

## Status

- [x] Board enumerates on `COM4`
- [x] Firmware base vendored (`08_Audio_Test` + `esp_wifi_bsp`)
- [x] Repo scaffolded (`server/`, `voice-channel/` skeletons)
- [ ] Install ESP-IDF v5.5 toolchain (VS Code extension)
- [ ] Build + flash the stock demo to confirm the board works
- [ ] Write the buddy firmware app (PTT capture → POST, `/display` → render)
- [ ] Deploy `server/` on max (faster-whisper container)
- [ ] Deploy `voice-channel/` + verify turn-injection into `ss-channels`

See [`firmware/buddy/main/secrets.h.example`](firmware/buddy/main/secrets.h.example) for the
WiFi/server config you'll copy to `secrets.h` (gitignored) before flashing.
