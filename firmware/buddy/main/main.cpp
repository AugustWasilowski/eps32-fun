// Claude Buddy firmware — Stage 3: PTT mic -> WAV -> POST  +  e-paper display.
//
// Push-to-talk: hold BOOT (GPIO0), speak, release -> record from the ES8311 mic,
// downmix stereo->mono, wrap in a WAV header, POST to ${BUDDY_SERVER_URL}/transcribe
// (X-Buddy-Token). The plain-text response is the transcript.
//
// Display: a 3-zone LVGL UI on the 200x200 e-paper — token usage (top), context-%
// bar (middle), last transcript (bottom, word-wrapped). A poll task GETs
// ${BUDDY_SERVER_URL}/display every DISPLAY_POLL_SECONDS and once right after a
// transcript. LVGL flushes through Waveshare's e-paper partial-refresh path.
//
// Audio + e-paper init is Waveshare's, kept verbatim via user_app_init(): it powers
// the audio rail (GPIO42), inits I2C/e-paper/button, and opens the codec record
// stream at 16 kHz / 2 ch / 16-bit.
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "lvgl.h"

#include "user_app.h"
#include "user_config.h"
#include "audio_bsp.h"
#include "esp_wifi_bsp.h"
#include "gui_guider.h"   // LV_FONT_DECLARE(lv_font_Bold_20 / lv_font_montserratMedium_16)
#include "secrets.h"

static const char *TAG = "buddy";

// --- Audio (codec opened at 16 kHz, 2 ch, 16-bit; see audio_bsp::audio_play_init). ---
#define SAMPLE_RATE_HZ   16000
#define NUM_CHANNELS     2
#define BYTES_PER_SAMPLE 2
#define BYTES_PER_FRAME  (NUM_CHANNELS * BYTES_PER_SAMPLE)        // 4 bytes (L+R)
#define BYTES_PER_SEC    (SAMPLE_RATE_HZ * BYTES_PER_FRAME)       // 64000 B/s

#define CHUNK_BYTES      (BYTES_PER_SEC / 8)                      // 8000 B (~0.125 s)
#define MAX_RECORD_SEC   10
#define RECORD_BUF_BYTES (MAX_RECORD_SEC * BYTES_PER_SEC)         // 640000 B (stereo)
#define WAV_HDR_BYTES    44
#define WAV_BUF_BYTES    (WAV_HDR_BYTES + RECORD_BUF_BYTES / 2)   // mono + header
#define MIN_SEND_BYTES   (BYTES_PER_SEC / 4)                      // 0.25 s stereo

// One automatic record+POST at boot (no button) to validate the HTTP path.
// Set to 0 once the device->server path is confirmed.
#define SELFTEST_AT_BOOT 0

// ============================ LVGL / e-paper display ============================

extern epaper_driver_display *driver;  // created by user_app_init()

static SemaphoreHandle_t s_lvgl_mux = NULL;
static SemaphoreHandle_t s_display_poke = NULL;  // give -> poll task does an immediate GET

// UI widgets we update at runtime.
static lv_obj_t *s_lbl_tokens = NULL;
static lv_obj_t *s_lbl_ctx = NULL;
static lv_obj_t *s_bar_ctx = NULL;
static lv_obj_t *s_lbl_you = NULL;   // your last transcript (1 line, truncated)
static lv_obj_t *s_lbl_max = NULL;   // Max's reply (wrapped, main content)

// Last-rendered text per zone, so we only repaint (slow e-paper refresh) on change.
static char s_last_tokens[64] = "";
static char s_last_ctx[32] = "";
static char s_last_you[288] = "";
static char s_last_max[320] = "";

// Flush LVGL's render buffer to the e-paper (full-screen partial refresh).
// Identical pixel path to Waveshare's audio-test demo.
static void epaper_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    driver->EPD_Clear();
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            driver->EPD_DrawColorPixel(x, y, color);
            buffer++;
        }
    }
    driver->EPD_DisplayPart();
    lv_disp_flush_ready(drv);
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS); }

static bool lvgl_lock(int timeout_ms)
{
    const TickType_t t = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, t) == pdTRUE;
}
static void lvgl_unlock(void) { xSemaphoreGive(s_lvgl_mux); }

static void lvgl_port_task(void *arg)
{
    uint32_t delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (lvgl_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        else if (delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// Build the 3-zone UI. Caller need not hold the lock (called before tasks start).
static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Zone 1: tokens (top, bold) — cost folded in when available.
    s_lbl_tokens = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_tokens, &lv_font_Bold_20, 0);
    lv_obj_set_style_text_color(s_lbl_tokens, lv_color_black(), 0);
    lv_obj_set_pos(s_lbl_tokens, 4, 2);
    lv_label_set_text(s_lbl_tokens, "Tokens: --");

    // Zone 2: context-window % label + bar.
    s_lbl_ctx = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_ctx, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(s_lbl_ctx, lv_color_black(), 0);
    lv_obj_set_pos(s_lbl_ctx, 4, 26);
    lv_label_set_text(s_lbl_ctx, "Context: --");

    s_bar_ctx = lv_bar_create(scr);
    lv_obj_set_pos(s_bar_ctx, 4, 46);
    lv_obj_set_size(s_bar_ctx, 192, 12);
    lv_obj_set_style_bg_color(s_bar_ctx, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bar_ctx, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar_ctx, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_ctx, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_ctx, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_ctx, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar_ctx, 0, 100);
    lv_bar_set_value(s_bar_ctx, 0, LV_ANIM_OFF);

    // Divider.
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_pos(line, 4, 64);
    lv_obj_set_size(line, 192, 2);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    // Zone 3a: what you said (one line, ellipsized).
    s_lbl_you = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_you, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(s_lbl_you, lv_color_black(), 0);
    lv_obj_set_pos(s_lbl_you, 4, 70);
    lv_obj_set_size(s_lbl_you, 192, 20);
    lv_label_set_long_mode(s_lbl_you, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_you, "You: (say something)");

    // Zone 3b: Max's reply (wrapped, fills the rest; … if too long for the panel).
    s_lbl_max = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lbl_max, &lv_font_montserratMedium_16, 0);
    lv_obj_set_style_text_color(s_lbl_max, lv_color_black(), 0);
    lv_obj_set_pos(s_lbl_max, 4, 92);
    lv_obj_set_size(s_lbl_max, 192, 104);
    lv_label_set_long_mode(s_lbl_max, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lbl_max, "Max: --");
}

// Initialize LVGL and register the e-paper display driver. user_app_init() must
// have already run EPD_Init()/EPD_Init_Partial().
static void display_init(void)
{
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;

    lv_init();
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EPD_WIDTH * EPD_HEIGHT);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EPD_WIDTH;
    disp_drv.ver_res = EPD_HEIGHT;
    disp_drv.flush_cb = epaper_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1;  // must be 1 for this panel
    lv_disp_drv_register(&disp_drv);

    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lvgl_tick_cb;
    tick_args.name = "lvgl_tick";
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

    s_lvgl_mux = xSemaphoreCreateMutex();
    assert(s_lvgl_mux);

    if (lvgl_lock(-1)) {
        build_ui();
        lvgl_unlock();
    }
    xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", 8 * 1024, NULL, 4, NULL, 1);
}

// Update the "You said" line immediately after a POST response (before the poll
// catches up). Guarded so we don't trigger a redundant e-paper refresh.
static void display_set_transcript(const char *txt)
{
    if (!s_lbl_you) return;
    char buf[288];
    snprintf(buf, sizeof(buf), "You: %s", (txt && txt[0]) ? txt : "(empty)");
    if (lvgl_lock(1000)) {
        if (strcmp(s_last_you, buf) != 0) {
            lv_label_set_text(s_lbl_you, buf);
            strncpy(s_last_you, buf, sizeof(s_last_you) - 1);
            s_last_you[sizeof(s_last_you) - 1] = '\0';
        }
        lvgl_unlock();
    }
}

// ============================ HTTP: /transcribe + /display ============================

// Write a 44-byte canonical PCM WAV header (little-endian; ESP32 is LE).
static void write_wav_header(uint8_t *h, uint32_t data_bytes,
                             uint32_t rate, uint16_t channels, uint16_t bits)
{
    uint32_t byte_rate = rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;
    uint32_t riff_size = 36 + data_bytes;
    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &riff_size, 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt_size = 16; memcpy(h + 16, &fmt_size, 4);
    uint16_t pcm = 1;       memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

static double rms_int16(const uint8_t *buf, size_t len)
{
    const int16_t *s = (const int16_t *)buf;
    size_t n = len / BYTES_PER_SAMPLE;
    if (n == 0) return 0.0;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) { double v = (double)s[i]; acc += v * v; }
    return sqrt(acc / (double)n);
}

static inline bool ptt_pressed(void) { return gpio_get_level(BOOT_BUTTON_PIN) == 0; }

// --- Audible feedback (ES8311 speaker; codec already open for playback). ---
#define BEEP_MAX_MS    200
#define BEEP_BUF_BYTES ((SAMPLE_RATE_HZ / 1000) * BEEP_MAX_MS * BYTES_PER_FRAME)
static uint8_t *s_beep_buf = NULL;  // PSRAM, allocated in app_main

// Serializes codec access so TTS playback (POST /play) never collides with mic
// recording or beeps. Taken around a PTT cycle and around a /play request.
static SemaphoreHandle_t s_audio_mux = NULL;

// Inbound TTS (POST /play): whole body buffered in PSRAM, then mono->stereo to codec.
#define MAX_TTS_BYTES    (1024 * 1024)   // ~32 s of 16 kHz mono 16-bit
#define TTS_STEREO_FRAMES 2048
static uint8_t *s_tts_buf = NULL;        // raw uploaded bytes
static uint8_t *s_tts_stereo = NULL;     // mono->stereo scratch (TTS_STEREO_FRAMES*4)

// Play a sine tone (Hz, ms) to the speaker, with a short fade in/out to avoid
// clicks. Blocking. Stereo 16-bit at SAMPLE_RATE_HZ (matches the open codec).
static void play_tone(int freq, int ms, int amp)
{
    if (!s_beep_buf) return;
    if (ms > BEEP_MAX_MS) ms = BEEP_MAX_MS;
    size_t frames = (size_t)SAMPLE_RATE_HZ * ms / 1000;
    size_t ramp = frames / 8 + 1;
    int16_t *s = (int16_t *)s_beep_buf;
    for (size_t i = 0; i < frames; i++) {
        double env = 1.0;
        if (i < ramp) env = (double)i / ramp;
        else if (i > frames - ramp) env = (double)(frames - i) / ramp;
        int16_t v = (int16_t)(amp * env * sin(2.0 * 3.14159265358979 * freq * i / SAMPLE_RATE_HZ));
        s[2 * i] = v;
        s[2 * i + 1] = v;
    }
    audio_playback_write(s_beep_buf, frames * BYTES_PER_FRAME);
}

static void beep_start(void) { play_tone(1047, 70, 12000); }                       // high: recording
static void beep_stop(void)  { play_tone(660, 70, 12000); }                        // low: stopped
static void beep_ok(void)    { play_tone(1047, 60, 12000); play_tone(1568, 90, 12000); } // rising: sent
static void beep_fail(void)  { play_tone(330, 220, 12000); }                       // low long: failed
static void beep_incoming(void) { play_tone(988, 70, 11000); play_tone(1319, 120, 11000); } // rising: incoming TTS

// POST a WAV to /transcribe. On HTTP 200, copies the transcript into out (size
// out_sz) and returns the status code; returns negative on transport error.
static int post_wav(const uint8_t *wav, size_t wav_len, char *out, size_t out_sz)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/transcribe", BUDDY_SERVER_URL);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Buddy-Token", BUDDY_TOKEN);

    ESP_LOGI(TAG, "POST %s  (%u bytes)", url, (unsigned)wav_len);
    esp_err_t err = esp_http_client_open(client, wav_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }
    int written = esp_http_client_write(client, (const char *)wav, wav_len);
    if (written != (int)wav_len) {
        ESP_LOGE(TAG, "http write short: %d/%u", written, (unsigned)wav_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    int total = 0, r;
    while (total < (int)out_sz - 1 &&
           (r = esp_http_client_read(client, out + total, out_sz - 1 - total)) > 0) {
        total += r;
    }
    out[total < 0 ? 0 : total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status == 200) ESP_LOGI(TAG, "transcript (200): \"%s\"", out);
    else               ESP_LOGW(TAG, "POST status %d: \"%s\"", status, out);
    return status;
}

// Downmix stereo->mono, wrap in WAV, POST, and on success update the transcript
// zone and poke the display poll task to refresh usage/context. Returns the HTTP
// status (200 on success), or 0 if it didn't send, or negative on transport error.
static int send_recording(const uint8_t *record_buf, size_t total, uint8_t *wav_buf)
{
    if (total < MIN_SEND_BYTES) { ESP_LOGW(TAG, "  too short (<0.25s) — not sending."); return 0; }
    if (!espwifi_is_connected()) { ESP_LOGW(TAG, "  WiFi not connected — not sending."); return 0; }

    size_t frames = total / BYTES_PER_FRAME;
    const int16_t *src = (const int16_t *)record_buf;
    int16_t *dst = (int16_t *)(wav_buf + WAV_HDR_BYTES);
    for (size_t i = 0; i < frames; i++) {
        int32_t l = src[2 * i], rr = src[2 * i + 1];
        dst[i] = (int16_t)((l + rr) / 2);
    }
    size_t mono_bytes = frames * BYTES_PER_SAMPLE;
    write_wav_header(wav_buf, mono_bytes, SAMPLE_RATE_HZ, 1, 16);

    char transcript[256];
    int status = post_wav(wav_buf, WAV_HDR_BYTES + mono_bytes, transcript, sizeof(transcript));
    if (status == 200) {
        display_set_transcript(transcript);
        if (s_display_poke) xSemaphoreGive(s_display_poke);  // refresh tokens/context now
    }
    return status;
}

static size_t capture_bytes(uint8_t *record_buf, uint8_t *chunk, size_t want)
{
    if (want > RECORD_BUF_BYTES) want = RECORD_BUF_BYTES;
    size_t got = 0;
    while (got < want) {
        size_t n = CHUNK_BYTES;
        if (got + n > want) n = want - got;
        audio_playback_read(chunk, n);
        memcpy(record_buf + got, chunk, n);
        got += n;
    }
    return got;
}

// Abbreviate large counts so they fit the 200px panel (123456 -> "123.5k").
static void abbrev_count(long n, char *out, size_t sz)
{
    if (n < 0)            snprintf(out, sz, "--");
    else if (n < 1000)    snprintf(out, sz, "%ld", n);
    else if (n < 1000000) snprintf(out, sz, "%.1fk", n / 1000.0);
    else                  snprintf(out, sz, "%.2fM", n / 1000000.0);
}

// GET /display, parse JSON, and update the token/context zones.
static void http_get_display(void)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/display", BUDDY_SERVER_URL);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "X-Buddy-Token", BUDDY_TOKEN);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET /display open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char body[512];
    int total = 0, r;
    while (total < (int)sizeof(body) - 1 &&
           (r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
    }
    body[total < 0 ? 0 : total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) { ESP_LOGW(TAG, "GET /display status %d", status); return; }

    cJSON *root = cJSON_Parse(body);
    if (!root) { ESP_LOGW(TAG, "GET /display: bad JSON"); return; }

    cJSON *j_tokens = cJSON_GetObjectItem(root, "tokens");
    cJSON *j_cost = cJSON_GetObjectItem(root, "cost");
    cJSON *j_ctx = cJSON_GetObjectItem(root, "context_pct");
    cJSON *j_txt = cJSON_GetObjectItem(root, "last_transcript");
    cJSON *j_max = cJSON_GetObjectItem(root, "max_reply");

    // Tokens (+ cost folded in when present).
    char tokens_str[64];
    if (cJSON_IsNumber(j_tokens)) {
        char abbr[24];
        abbrev_count((long)j_tokens->valuedouble, abbr, sizeof(abbr));
        if (cJSON_IsNumber(j_cost))
            snprintf(tokens_str, sizeof(tokens_str), "Tokens: %s  $%.2f", abbr, j_cost->valuedouble);
        else
            snprintf(tokens_str, sizeof(tokens_str), "Tokens: %s", abbr);
    } else {
        snprintf(tokens_str, sizeof(tokens_str), "Tokens: --");
    }

    int ctx_val = 0;
    char ctx_str[32];
    if (cJSON_IsNumber(j_ctx)) {
        ctx_val = (int)(j_ctx->valuedouble + 0.5);
        if (ctx_val < 0) ctx_val = 0;
        if (ctx_val > 100) ctx_val = 100;
        snprintf(ctx_str, sizeof(ctx_str), "Context: %d%%", ctx_val);
    } else {
        snprintf(ctx_str, sizeof(ctx_str), "Context: --");
    }

    const char *txt = (cJSON_IsString(j_txt) && j_txt->valuestring) ? j_txt->valuestring : "";
    const char *mx = (cJSON_IsString(j_max) && j_max->valuestring) ? j_max->valuestring : "";
    char you_buf[288], max_buf[320];
    snprintf(you_buf, sizeof(you_buf), "You: %s", txt[0] ? txt : "(say something)");
    snprintf(max_buf, sizeof(max_buf), "Max: %s", mx[0] ? mx : "--");

    // Only repaint changed zones — each change is a (slow) e-paper refresh.
    if (lvgl_lock(1000)) {
        if (strcmp(s_last_tokens, tokens_str) != 0) {
            lv_label_set_text(s_lbl_tokens, tokens_str);
            strncpy(s_last_tokens, tokens_str, sizeof(s_last_tokens) - 1);
        }
        if (strcmp(s_last_ctx, ctx_str) != 0) {
            lv_label_set_text(s_lbl_ctx, ctx_str);
            lv_bar_set_value(s_bar_ctx, ctx_val, LV_ANIM_OFF);
            strncpy(s_last_ctx, ctx_str, sizeof(s_last_ctx) - 1);
        }
        if (strcmp(s_last_you, you_buf) != 0) {
            lv_label_set_text(s_lbl_you, you_buf);
            strncpy(s_last_you, you_buf, sizeof(s_last_you) - 1);
        }
        if (strcmp(s_last_max, max_buf) != 0) {
            lv_label_set_text(s_lbl_max, max_buf);
            strncpy(s_last_max, max_buf, sizeof(s_last_max) - 1);
        }
        lvgl_unlock();
    }
    cJSON_Delete(root);
}

// Tell max where to reach this buddy's /play server (IP is DHCP). Cheap; called
// each poll cycle so it self-heals across DHCP changes or a server restart.
static void register_with_max(void)
{
    char ip[16];
    if (!espwifi_get_ip(ip, sizeof(ip))) return;
    char url[160], body[96];
    snprintf(url, sizeof(url), "%s/register", BUDDY_SERVER_URL);
    int blen = snprintf(body, sizeof(body), "{\"ip\":\"%s\",\"port\":8080}", ip);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 8000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Buddy-Token", BUDDY_TOKEN);
    if (esp_http_client_open(client, blen) == ESP_OK) {
        esp_http_client_write(client, body, blen);
        esp_http_client_fetch_headers(client);
        static bool logged = false;
        if (!logged) { ESP_LOGI(TAG, "registered with max: %s:8080", ip); logged = true; }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

// Periodically refresh the display; also refresh immediately when poked.
static void display_poll_task(void *arg)
{
    for (;;) {
        if (espwifi_is_connected()) {
            register_with_max();
            http_get_display();
        }
        // Wait up to DISPLAY_POLL_SECONDS, or wake early when a transcript pokes us.
        if (xSemaphoreTake(s_display_poke, pdMS_TO_TICKS(DISPLAY_POLL_SECONDS * 1000)) == pdTRUE) {
            // A transcript was just sent — fast-poll so Max's reply shows promptly,
            // stopping as soon as max_reply changes (guards prevent extra refreshes).
            char before[sizeof(s_last_max)];
            strncpy(before, s_last_max, sizeof(before));
            for (int i = 0; i < 15 && espwifi_is_connected(); i++) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                http_get_display();
                if (strcmp(s_last_max, before) != 0) break;  // Max replied
            }
        }
    }
}

// ============================ TTS playback: POST /play ============================

// Find the start of PCM data in a WAV buffer (offset after the "data" subchunk
// header), scanning the first bytes; falls back to the canonical 44.
static int wav_data_offset(const uint8_t *b, int n)
{
    for (int i = 12; i + 8 <= n && i < 512; i++) {
        if (memcmp(b + i, "data", 4) == 0) return i + 8;
    }
    return 44;
}

// Play 16-bit PCM through the codec (open at 16 kHz, 2 ch). Mono is duplicated to
// stereo; stereo is written as-is. Blocking.
static void play_pcm16(const uint8_t *pcm, size_t bytes, int channels)
{
    if (channels >= 2) {
        audio_playback_write((void *)pcm, (bytes / BYTES_PER_FRAME) * BYTES_PER_FRAME);
        return;
    }
    const int16_t *s = (const int16_t *)pcm;
    size_t samples = bytes / 2;
    int16_t *out = (int16_t *)s_tts_stereo;
    size_t i = 0;
    while (i < samples) {
        size_t batch = samples - i;
        if (batch > TTS_STEREO_FRAMES) batch = TTS_STEREO_FRAMES;
        for (size_t k = 0; k < batch; k++) {
            int16_t v = s[i + k];
            out[2 * k] = v;
            out[2 * k + 1] = v;
        }
        audio_playback_write(out, batch * BYTES_PER_FRAME);
        i += batch;
    }
}

// POST /play — body is 16 kHz 16-bit PCM (WAV or raw, mono or stereo). Gated by
// X-Buddy-Token. Buffers the body, then plays it through the speaker.
static esp_err_t play_handler(httpd_req_t *req)
{
    char tok[80] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Buddy-Token", tok, sizeof(tok)) != ESP_OK ||
        strcmp(tok, BUDDY_TOKEN) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "forbidden");
        return ESP_FAIL;
    }
    int len = req->content_len;
    if (len <= 0 || len > MAX_TTS_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty or too large");
        return ESP_FAIL;
    }

    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)s_tts_buf + got, len - got);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        got += n;
    }

    // Parse WAV header if present; else treat as raw 16 kHz mono PCM.
    int channels = 1, offset = 0;
    if (got >= 28 && memcmp(s_tts_buf, "RIFF", 4) == 0) {
        channels = s_tts_buf[22] | (s_tts_buf[23] << 8);
        int rate = s_tts_buf[24] | (s_tts_buf[25] << 8) |
                   (s_tts_buf[26] << 16) | (s_tts_buf[27] << 24);
        offset = wav_data_offset(s_tts_buf, got);
        if (rate != SAMPLE_RATE_HZ)
            ESP_LOGW(TAG, "/play: WAV rate %d != %d Hz — will sound off; resample on max",
                     rate, SAMPLE_RATE_HZ);
        if (channels < 1) channels = 1;
    }

    ESP_LOGI(TAG, "/play: %d bytes, %d ch, playing...", got - offset, channels);
    xSemaphoreTake(s_audio_mux, portMAX_DELAY);
    beep_incoming();                      // heads-up chime before the message
    vTaskDelay(pdMS_TO_TICKS(120));       // small gap so it doesn't run into speech
    play_pcm16(s_tts_buf + offset, got - offset, channels);
    xSemaphoreGive(s_audio_mux);

    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static void start_play_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.stack_size = 8192;
    config.recv_wait_timeout = 20;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t play_uri = {};
        play_uri.uri = "/play";
        play_uri.method = HTTP_POST;
        play_uri.handler = play_handler;
        httpd_register_uri_handler(server, &play_uri);
        ESP_LOGI(TAG, "play server up: POST http://<buddy-ip>:8080/play");
    } else {
        ESP_LOGE(TAG, "failed to start play server");
    }
}

// ============================ app_main ============================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Claude Buddy — Stage 3 (PTT POST + e-paper display)");

    // Waveshare init: powers audio rail, inits i2c/epaper/button, opens codec.
    user_app_init();

    // LVGL display on top of the (already-initialized) e-paper driver.
    display_init();

    // WiFi (creds from secrets.h).
    espwifi_Init(WIFI_SSID, WIFI_PASSWORD);
    if (espwifi_wait_connected(20000)) {
        ESP_LOGI(TAG, "WiFi connected. Server: %s", BUDDY_SERVER_URL);
    } else {
        ESP_LOGW(TAG, "WiFi NOT connected within 20s — will keep retrying in background.");
    }

    uint8_t *record_buf = (uint8_t *)heap_caps_malloc(RECORD_BUF_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *wav_buf = (uint8_t *)heap_caps_malloc(WAV_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_beep_buf = (uint8_t *)heap_caps_malloc(BEEP_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_tts_buf = (uint8_t *)heap_caps_malloc(MAX_TTS_BYTES, MALLOC_CAP_SPIRAM);
    s_tts_stereo = (uint8_t *)heap_caps_malloc(TTS_STEREO_FRAMES * BYTES_PER_FRAME, MALLOC_CAP_SPIRAM);
    assert(record_buf && chunk && wav_buf && s_beep_buf && s_tts_buf && s_tts_stereo &&
           "failed to allocate PSRAM buffers");

    s_audio_mux = xSemaphoreCreateMutex();
    assert(s_audio_mux);

    // HTTP server so max's Piper TTS can push voice messages to the speaker.
    if (espwifi_is_connected()) start_play_server();

    // Start the display poll loop (binary semaphore: poke = immediate refresh).
    s_display_poke = xSemaphoreCreateBinary();
    assert(s_display_poke);
    xTaskCreatePinnedToCore(display_poll_task, "display_poll", 6 * 1024, NULL, 3, NULL, 1);

#if SELFTEST_AT_BOOT
    if (espwifi_is_connected()) {
        ESP_LOGI(TAG, "SELF-TEST: recording ~1s and POSTing (no button needed)...");
        size_t got = capture_bytes(record_buf, chunk, SAMPLE_RATE_HZ * BYTES_PER_FRAME);
        ESP_LOGI(TAG, "SELF-TEST: captured %u bytes, rms=%.0f", (unsigned)got, rms_int16(record_buf, got));
        send_recording(record_buf, got, wav_buf);
    }
#else
    (void)capture_bytes; (void)rms_int16;  // referenced by self-test / PTT only
#endif

    ESP_LOGI(TAG, "Ready. Hold BOOT (GPIO%d) and speak; release to stop.", BOOT_BUTTON_PIN);

    for (;;) {
        if (!ptt_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Hold the codec for the whole cycle so an incoming /play can't interleave.
        xSemaphoreTake(s_audio_mux, portMAX_DELAY);
        ESP_LOGI(TAG, "REC start");
        beep_start();
        size_t total = 0;
        double peak_rms = 0.0;
        while (ptt_pressed() && total < RECORD_BUF_BYTES) {
            size_t want = CHUNK_BYTES;
            if (total + want > RECORD_BUF_BYTES) want = RECORD_BUF_BYTES - total;
            audio_playback_read(chunk, want);
            memcpy(record_buf + total, chunk, want);
            total += want;
            double r = rms_int16(chunk, want);
            if (r > peak_rms) peak_rms = r;
        }
        ESP_LOGI(TAG, "REC done: %.2f s, %u bytes, peak rms=%.0f",
                 (float)total / BYTES_PER_SEC, (unsigned)total, peak_rms);
        beep_stop();

        int status = send_recording(record_buf, total, wav_buf);
        if (status == 200) beep_ok();
        else               beep_fail();
        xSemaphoreGive(s_audio_mux);

        vTaskDelay(pdMS_TO_TICKS(150));  // debounce release edge
    }
}
