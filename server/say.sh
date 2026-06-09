#!/usr/bin/env bash
# say.sh — speak text on the ESP32 "buddy" via max's Piper TTS.
#
#   ./say.sh "your message here"
#
# Pipeline: Piper (:5050, 22050 Hz mono WAV) -> ffmpeg resample to 16 kHz mono
# raw PCM -> POST to the buddy's /play. The buddy's IP is looked up from the
# buddy server's /buddy endpoint (the device registers its DHCP IP there).
set -euo pipefail

TEXT="${1:-}"
if [ -z "$TEXT" ]; then echo "usage: $0 <text>" >&2; exit 1; fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOKEN="$(grep -E '^BUDDY_TOKEN=' "$DIR/.env" | cut -d= -f2-)"
PIPER_URL="${PIPER_URL:-http://localhost:5050}"
SERVER_URL="${SERVER_URL:-http://localhost:8810}"

PLAY_URL="$(curl -sf -H "X-Buddy-Token: $TOKEN" "$SERVER_URL/buddy" | jq -r '.play_url')"
if [ -z "$PLAY_URL" ] || [ "$PLAY_URL" = "null" ]; then
  echo "buddy has not registered its IP yet (is it powered on / on WiFi?)" >&2
  exit 1
fi

# Header values must be single-line; collapse any newlines for the display text.
HDR_TEXT="$(printf '%s' "$TEXT" | tr '\n\r' '  ')"

payload="$(jq -nc --arg t "$TEXT" '{text:$t}')"
curl -sf -X POST -H "Content-Type: application/json" --data "$payload" "$PIPER_URL" \
  | ffmpeg -loglevel error -i - -ar 16000 -ac 1 -f s16le - \
  | curl -sf --data-binary @- \
      -H "X-Buddy-Token: $TOKEN" -H "Content-Type: application/octet-stream" \
      -H "X-Buddy-Text: $HDR_TEXT" \
      "$PLAY_URL" >/dev/null
echo "spoke on buddy ($PLAY_URL): $TEXT"
