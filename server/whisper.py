"""faster-whisper wrapper. Lazy-loads the model on first use so import is cheap.

Env:
  WHISPER_MODEL    default "small.en"  (base.en / small.en / medium.en ...)
  WHISPER_DEVICE   default "cuda"      ("cpu" to force CPU)
  WHISPER_COMPUTE  default "float16"   ("int8" for CPU)
"""
import os
import threading

from faster_whisper import WhisperModel

_model: WhisperModel | None = None
_lock = threading.Lock()


def get_model() -> WhisperModel:
    global _model
    if _model is None:
        with _lock:
            if _model is None:
                _model = WhisperModel(
                    os.environ.get("WHISPER_MODEL", "small.en"),
                    device=os.environ.get("WHISPER_DEVICE", "cuda"),
                    compute_type=os.environ.get("WHISPER_COMPUTE", "float16"),
                    download_root=os.environ.get("WHISPER_CACHE", "/models"),
                )
    return _model


def transcribe_wav(path: str) -> str:
    """Transcribe a WAV file path -> plain text. VAD filter trims silence/noise."""
    segments, _info = get_model().transcribe(
        path,
        language=os.environ.get("WHISPER_LANG", "en"),
        vad_filter=True,
        beam_size=5,
    )
    return " ".join(seg.text.strip() for seg in segments).strip()
