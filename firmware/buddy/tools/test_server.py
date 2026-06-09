"""Throwaway local stand-in for the max server, to validate the device's POST path.

No dependencies (stdlib only). Listens on 0.0.0.0:8810 and implements just enough:
  POST /transcribe  -> checks X-Buddy-Token, saves body to tools/recv/<n>.wav,
                       prints WAV info, returns a fake transcript as text/plain.
  GET  /display     -> returns dummy JSON in the real /display shape.

Run:  python tools/test_server.py
Stop: Ctrl+C.  The token must match BUDDY_TOKEN in main/secrets.h.
"""
import json
import os
import struct
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = "dev-buddy-token-7f3a9c2e1b"
PORT = 8810
RECV_DIR = os.path.join(os.path.dirname(__file__), "recv")
os.makedirs(RECV_DIR, exist_ok=True)
_counter = {"n": 0}
# Mutable display state so /display reflects what was last POSTed (mimics max).
STATE = {"last_transcript": "hello from the test server"}


def _wav_summary(path: str) -> str:
    try:
        with wave.open(path, "rb") as w:
            ch, sw, fr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
            secs = n / fr if fr else 0
            return f"{ch}ch {sw*8}bit {fr}Hz, {n} frames, {secs:.2f}s"
    except Exception as exc:  # noqa: BLE001
        return f"(not a parseable WAV: {exc})"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quieter default logging
        pass

    def _reject(self, code, msg):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(msg.encode())

    def do_POST(self):
        if self.path.rstrip("/") != "/transcribe":
            return self._reject(404, "not found")
        if self.headers.get("X-Buddy-Token") != TOKEN:
            print(f"[server] REJECTED: bad/missing X-Buddy-Token "
                  f"(got {self.headers.get('X-Buddy-Token')!r})")
            return self._reject(403, "forbidden")

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        _counter["n"] += 1
        path = os.path.join(RECV_DIR, f"{_counter['n']:03d}.wav")
        with open(path, "wb") as f:
            f.write(body)
        ctype = self.headers.get("Content-Type")
        print(f"[server] POST /transcribe  {len(body)} bytes  Content-Type={ctype}")
        print(f"[server]   saved {path}  ->  {_wav_summary(path)}")

        transcript = f"test #{_counter['n']}: {len(body)//2000/16:.1f}s of audio received"
        STATE["last_transcript"] = transcript
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(transcript.encode())

    def do_GET(self):
        if self.path.rstrip("/") != "/display":
            return self._reject(404, "not found")
        if self.headers.get("X-Buddy-Token") != TOKEN:
            return self._reject(403, "forbidden")
        payload = {
            "tokens": 123456,
            "cost": 1.23,
            "context_pct": 42.0,
            "last_transcript": STATE["last_transcript"],
        }
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(payload).encode())


if __name__ == "__main__":
    print(f"[server] listening on 0.0.0.0:{PORT}  (token={TOKEN})")
    print(f"[server] saving WAVs to {RECV_DIR}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
