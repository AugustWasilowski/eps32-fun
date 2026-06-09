# firmware/

ESP-IDF (v5.5.0+) firmware for the Waveshare ESP32-S3-ePaper-1.54 board.

## `buddy/` — the project

Derived from Waveshare's `02_Example/ESP-IDF/V2/08_Audio_Test`, which already wires the hard parts
as separate BSP components:

- `audio_bsp` / `codec_board` — ES8311 codec (I2S mic in + speaker out)
- `epaper_driver_bsp` — 1.54" 200×200 e-paper driver
- `button_bsp` — the onboard button (push-to-talk)
- `i2c_bsp`, `board_power_bsp`, `ui_bsp`, `user_app` — board bring-up + LVGL UI
- `esp_wifi_bsp` — **added** from `06_WIFI_STA` for WiFi station + HTTP client

Right now `main/main.cpp` is still the stock audio+e-paper demo — flashing it is a good first
hardware smoke test. The buddy app (PTT capture → `POST /transcribe`, `GET /display` → render)
gets built on top of these components.

### Build & flash (VS Code ESP-IDF extension, or the ESP-IDF terminal)

```
cp main/secrets.h.example main/secrets.h   # then edit
idf.py set-target esp32s3
idf.py -p COM4 flash monitor
```

`build/`, `managed_components/`, `sdkconfig`, and `secrets.h` are gitignored — `idf.py` regenerates
the first three; `idf.py reconfigure` pulls managed components from `idf_component.yml`.

## `waveshare-examples/` — reference

Unmodified Waveshare examples kept for wiring reference (`06_WIFI_STA`). The full demo set lives at
<https://github.com/waveshareteam/ESP32-S3-ePaper-1.54> under `02_Example/ESP-IDF/V2`.
