# CUDA + cuDNN runtime for faster-whisper (CTranslate2) GPU inference.
FROM nvidia/cuda:12.4.1-cudnn-runtime-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive PYTHONUNBUFFERED=1
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip curl ca-certificates \
    # node for `npx ccusage` (token usage). Drop these two lines to slim the image
    # and rely on the JSONL-summing fallback in usage.py instead.
    && curl -fsSL https://deb.nodesource.com/setup_20.x | bash - \
    && apt-get install -y --no-install-recommends nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY requirements.txt .
RUN pip3 install --no-cache-dir -r requirements.txt
COPY *.py .

ENV WHISPER_CACHE=/models
EXPOSE 8810
CMD ["uvicorn", "buddy_server:app", "--host", "0.0.0.0", "--port", "8810"]
