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
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_netif_sntp.h"
#include "cJSON.h"
#include "lvgl.h"

#include "user_app.h"
#include "user_config.h"
#include "audio_bsp.h"
#include "button_bsp.h"   // pwr_groups event group (PWR button = airplane toggle)
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

#define CHUNK_BYTES        (BYTES_PER_SEC / 8)                    // 8000 B stereo (~0.125 s)
#define MAX_RECORD_SEC     60
#define MONO_BYTES_PER_SEC (SAMPLE_RATE_HZ * BYTES_PER_SAMPLE)    // 32000 B/s mono
#define REC_MONO_BYTES     (MAX_RECORD_SEC * MONO_BYTES_PER_SEC)  // mono PCM capacity
#define WAV_HDR_BYTES      44
// Single capture buffer: 44-byte WAV header + mono PCM (we downmix on the fly).
#define REC_BUF_BYTES      (WAV_HDR_BYTES + REC_MONO_BYTES)
#define MIN_SEND_MONO      (MONO_BYTES_PER_SEC / 4)               // 0.25 s mono

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
static char s_last_ctx[48] = "";
static char s_last_you[288] = "";
static char s_last_max[320] = "";        // text currently shown on the "Max:" line
static char s_last_server_max[320] = ""; // last max_reply seen from /display (so a
                                         // spoken /play text isn't clobbered by a poll)
static int  s_last_ctx_val = -1;         // last bar value rendered

// Buddy mode, cycled by the PWR button (shown on the Context line):
//   Max = live send to Max's Claude session;  Leo = live send to Leo (this PC);
//   Air = offline — queue to SD, drain later to whichever live mode is active.
typedef enum { MODE_MAX = 0, MODE_LEO = 1, MODE_AIR = 2 } buddy_mode_t;
static buddy_mode_t s_mode = MODE_MAX;
static inline const char *mode_name(void)   { return s_mode == MODE_MAX ? "Max" : s_mode == MODE_LEO ? "Leo" : "AIR"; }
// Destination tag sent to the server: max/leo = chat; note = notes.md (Air mode).
static inline const char *mode_target(void) { return s_mode == MODE_LEO ? "leo" : s_mode == MODE_AIR ? "note" : "max"; }

static int  s_queue_count = 0;
static char s_ctx_base[24] = "Context: --";  // "Context: NN%" from the last poll
static int  s_ctx_val = 0;                    // context % for the bar

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

// Compose the "Context:" line (base % from poll + AIR/queue status) and update
// the bar. Assumes the LVGL lock is held.
static void refresh_ctx_line_locked(void)
{
    char suf[24];
    if (s_queue_count > 0) snprintf(suf, sizeof(suf), "  %s q%d", mode_name(), s_queue_count);
    else                   snprintf(suf, sizeof(suf), "  %s", mode_name());
    char line[48];
    snprintf(line, sizeof(line), "%s%s", s_ctx_base, suf);
    if (strcmp(s_last_ctx, line) != 0) {
        lv_label_set_text(s_lbl_ctx, line);
        strncpy(s_last_ctx, line, sizeof(s_last_ctx) - 1);
        s_last_ctx[sizeof(s_last_ctx) - 1] = '\0';
    }
    if (s_ctx_val != s_last_ctx_val) {
        lv_bar_set_value(s_bar_ctx, s_ctx_val, LV_ANIM_OFF);
        s_last_ctx_val = s_ctx_val;
    }
}

static void refresh_ctx_line(void)
{
    if (lvgl_lock(1000)) { refresh_ctx_line_locked(); lvgl_unlock(); }
}

// Set the "Max:" line to spoken (/play) text. Does NOT touch s_last_server_max,
// so the next /display poll won't clobber it unless Max's reply actually changes.
static void display_set_max(const char *txt)
{
    if (!s_lbl_max) return;
    char buf[320];
    snprintf(buf, sizeof(buf), "Max: %s", (txt && txt[0]) ? txt : "(spoke)");
    if (lvgl_lock(1000)) {
        if (strcmp(s_last_max, buf) != 0) {
            lv_label_set_text(s_lbl_max, buf);
            strncpy(s_last_max, buf, sizeof(s_last_max) - 1);
            s_last_max[sizeof(s_last_max) - 1] = '\0';
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

// Current UNIX epoch (seconds), or 0 if the clock isn't SNTP-synced yet.
static long long now_epoch(void)
{
    time_t t = time(NULL);
    return (t > 1000000000) ? (long long)t : 0;
}

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
static void beep_queued(void) { play_tone(784, 80, 11000); play_tone(587, 110, 11000); }     // descending: saved offline

// POST a WAV to /transcribe. On HTTP 200, copies the transcript into out (size
// out_sz) and returns the status code; returns negative on transport error.
static int post_wav(const uint8_t *wav, size_t wav_len, const char *target, long long epoch,
                    char *out, size_t out_sz)
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
    esp_http_client_set_header(client, "X-Buddy-Target", target);
    if (epoch > 0) {
        char ts[24];
        snprintf(ts, sizeof(ts), "%lld", epoch);
        esp_http_client_set_header(client, "X-Buddy-Time", ts);
    }

    ESP_LOGI(TAG, "POST %s  (%u bytes, target=%s)", url, (unsigned)wav_len, target);
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

// POST a complete WAV buffer (header + PCM) to /transcribe; on 200 show the
// transcript and poke the poll task. Returns HTTP status (negative on error).
static int post_and_show(uint8_t *wav, size_t wav_len, const char *target, long long epoch)
{
    char transcript[256];
    int status = post_wav(wav, wav_len, target, epoch, transcript, sizeof(transcript));
    if (status == 200) {
        display_set_transcript(transcript);
        if (s_display_poke) xSemaphoreGive(s_display_poke);
    }
    return status;
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

    if (cJSON_IsNumber(j_ctx)) {
        int v = (int)(j_ctx->valuedouble + 0.5);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        s_ctx_val = v;
        snprintf(s_ctx_base, sizeof(s_ctx_base), "Context: %d%%", v);
    } else {
        s_ctx_val = 0;
        snprintf(s_ctx_base, sizeof(s_ctx_base), "Context: --");
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
        refresh_ctx_line_locked();  // composes base % + AIR/queue status, updates bar
        if (strcmp(s_last_you, you_buf) != 0) {
            lv_label_set_text(s_lbl_you, you_buf);
            strncpy(s_last_you, you_buf, sizeof(s_last_you) - 1);
        }
        // Only repaint "Max:" when the server's reply genuinely changed — so a
        // spoken /play text (set via display_set_max) isn't clobbered by polls.
        if (strcmp(s_last_server_max, mx) != 0) {
            lv_label_set_text(s_lbl_max, max_buf);
            strncpy(s_last_max, max_buf, sizeof(s_last_max) - 1);
            strncpy(s_last_server_max, mx, sizeof(s_last_server_max) - 1);
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

    // Optional: caller can pass the spoken text to show on the e-paper.
    char say_text[256];
    if (httpd_req_get_hdr_value_str(req, "X-Buddy-Text", say_text, sizeof(say_text)) == ESP_OK
        && say_text[0]) {
        display_set_max(say_text);
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

// ============================ SD card (TF slot) ============================
// 1-bit SDMMC on this board: CLK=39, CMD=41, D0=40 (Waveshare 04_SD_Card example).
#define SD_CLK_PIN  GPIO_NUM_39
#define SD_CMD_PIN  GPIO_NUM_41
#define SD_D0_PIN   GPIO_NUM_40
#define SD_MOUNT    "/sdcard"
#define QUEUE_DIR   SD_MOUNT "/queue"
static bool s_sd_ok = false;
static SemaphoreHandle_t s_sd_mux = NULL;   // serialize SD file ops
static uint32_t s_next_seq = 0;             // next queue filename number

static bool sdcard_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK_PIN;
    slot.cmd = SD_CMD_PIN;
    slot.d0 = SD_D0_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   // never reformat the user's card
    mcfg.max_files = 5;
    mcfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mcfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (card inserted & FAT32?)", esp_err_to_name(ret));
        return false;
    }
    uint64_t mb = ((uint64_t)card->csd.capacity * card->csd.sector_size) / (1024 * 1024);
    ESP_LOGI(TAG, "SD mounted at %s: %lluMB", SD_MOUNT, (unsigned long long)mb);
    return true;
}

static void sdcard_selftest(void)
{
    FILE *f = fopen(SD_MOUNT "/buddy_test.txt", "w");
    if (!f) { ESP_LOGW(TAG, "SD test: open-for-write failed"); return; }
    fprintf(f, "buddy sd ok\n");
    fclose(f);
    char line[32] = {0};
    f = fopen(SD_MOUNT "/buddy_test.txt", "r");
    if (f) { if (!fgets(line, sizeof(line), f)) line[0] = '\0'; fclose(f); }
    ESP_LOGI(TAG, "SD test: wrote+read back \"%s\"", line);
}

// Create the queue dir and learn the pending count + next sequence number.
static void queue_scan_init(void)
{
    mkdir(QUEUE_DIR, 0777);
    uint32_t next = 0;
    int count = 0;
    DIR *d = opendir(QUEUE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            unsigned n;
            if (sscanf(e->d_name, "%u_", &n) == 1) {
                count++;
                if (n + 1 > next) next = n + 1;
            }
        }
        closedir(d);
    }
    s_next_seq = next;
    s_queue_count = count;
    ESP_LOGI(TAG, "queue: %d pending, next seq %u", count, (unsigned)next);
}

// Save a complete WAV (header + PCM) to the SD queue for later upload. The
// filename encodes the destination + record epoch: NNNNNN_<target>_<epoch>.wav.
static bool save_to_queue(const uint8_t *wav, size_t len, const char *target, long long epoch)
{
    if (!s_sd_ok) return false;
    char path[80];
    bool ok = false;
    xSemaphoreTake(s_sd_mux, portMAX_DELAY);
    snprintf(path, sizeof(path), QUEUE_DIR "/%06u_%s_%lld.wav", (unsigned)s_next_seq, target, epoch);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        ok = (fwrite(wav, 1, len, fp) == len);
        fclose(fp);
    }
    if (ok) { s_next_seq++; s_queue_count++; }
    xSemaphoreGive(s_sd_mux);
    if (ok) {
        ESP_LOGI(TAG, "queued %s (%u bytes); %d pending", path, (unsigned)len, s_queue_count);
        refresh_ctx_line();
    } else {
        ESP_LOGW(TAG, "queue write failed: %s", path);
    }
    return ok;
}

// Stream a queued WAV file to /transcribe (no big buffer). On 200, show the
// transcript + poke. Returns HTTP status (negative on error).
static int post_file_and_show(const char *path, const char *target, long long epoch)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return -1;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char url[160];
    snprintf(url, sizeof(url), "%s/transcribe", BUDDY_SERVER_URL);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Buddy-Token", BUDDY_TOKEN);
    esp_http_client_set_header(client, "X-Buddy-Target", target);
    if (epoch > 0) {
        char ts[24];
        snprintf(ts, sizeof(ts), "%lld", epoch);
        esp_http_client_set_header(client, "X-Buddy-Time", ts);
    }

    if (esp_http_client_open(client, st.st_size) != ESP_OK) {
        fclose(fp); esp_http_client_cleanup(client); return -1;
    }
    uint8_t buf[2048];
    size_t n;
    bool werr = false;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (esp_http_client_write(client, (char *)buf, n) < 0) { werr = true; break; }
    }
    fclose(fp);
    if (werr) { esp_http_client_close(client); esp_http_client_cleanup(client); return -1; }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char resp[256];
    int total = 0, r;
    while (total < (int)sizeof(resp) - 1 &&
           (r = esp_http_client_read(client, resp + total, sizeof(resp) - 1 - total)) > 0) {
        total += r;
    }
    resp[total < 0 ? 0 : total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status == 200) {
        display_set_transcript(resp);
        if (s_display_poke) xSemaphoreGive(s_display_poke);
        ESP_LOGI(TAG, "drained %s -> \"%s\"", path, resp);
    } else {
        ESP_LOGW(TAG, "drain %s: status %d", path, status);
    }
    return status;
}

// Uploads queued recordings oldest-first whenever online and not in airplane mode.
static void drain_task(void *arg)
{
    for (;;) {
        if (s_sd_ok && s_queue_count > 0 && espwifi_is_connected()) {
            // Pick the oldest queued file (lowest seq), reading its target + epoch.
            unsigned oldest_n = 0xffffffffu;
            char target[16] = "max";
            long long epoch = 0;
            xSemaphoreTake(s_sd_mux, portMAX_DELAY);
            DIR *d = opendir(QUEUE_DIR);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    unsigned n; char tgt[16]; long long ep;
                    if (sscanf(e->d_name, "%u_%15[^_]_%lld.wav", &n, tgt, &ep) == 3 && n < oldest_n) {
                        oldest_n = n;
                        snprintf(target, sizeof(target), "%s", tgt);
                        epoch = ep;
                    }
                }
                closedir(d);
            }
            xSemaphoreGive(s_sd_mux);

            if (oldest_n != 0xffffffffu) {
                char path[96];
                snprintf(path, sizeof(path), QUEUE_DIR "/%06u_%s_%lld.wav", oldest_n, target, epoch);
                if (post_file_and_show(path, target, epoch) == 200) {
                    xSemaphoreTake(s_sd_mux, portMAX_DELAY);
                    remove(path);
                    if (s_queue_count > 0) s_queue_count--;
                    xSemaphoreGive(s_sd_mux);
                    refresh_ctx_line();
                    vTaskDelay(pdMS_TO_TICKS(400));  // brief gap, then next file
                    continue;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// PWR button (single click) cycles the mode: Max -> Leo -> Air -> Max.
static void pwr_button_task(void *arg)
{
    for (;;) {
        EventBits_t e = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, portMAX_DELAY);
        if (get_bit_button(e, 0)) {  // single click
            s_mode = (buddy_mode_t)((s_mode + 1) % 3);
            ESP_LOGI(TAG, "mode -> %s", mode_name());
            xSemaphoreTake(s_audio_mux, portMAX_DELAY);
            if (s_mode == MODE_MAX)      { play_tone(1047, 110, 11000); }                          // Max: one high
            else if (s_mode == MODE_LEO) { play_tone(1047, 70, 11000); play_tone(1319, 100, 11000); } // Leo: rising
            else                         { play_tone(587, 150, 11000); }                           // Air: one low
            xSemaphoreGive(s_audio_mux);
            refresh_ctx_line();
        }
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

    // Mount the TF/SD card (for offline capture queue) and self-test it.
    s_sd_ok = sdcard_mount();
    if (s_sd_ok) sdcard_selftest();

    // WiFi (creds from secrets.h).
    espwifi_Init(WIFI_SSID, WIFI_PASSWORD);
    if (espwifi_wait_connected(20000)) {
        ESP_LOGI(TAG, "WiFi connected. Server: %s", BUDDY_SERVER_URL);
        // Sync the clock so voice notes get the real record time (even if queued
        // offline and drained later). Best-effort; persists while powered.
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_netif_sntp_init(&sntp_cfg);
    } else {
        ESP_LOGW(TAG, "WiFi NOT connected within 20s — will keep retrying in background.");
    }

    // Single mono capture buffer (44-byte WAV header reserved up front) + a small
    // stereo read chunk. We downmix stereo->mono on the fly during capture.
    uint8_t *rec = (uint8_t *)heap_caps_malloc(REC_BUF_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    s_beep_buf = (uint8_t *)heap_caps_malloc(BEEP_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_tts_buf = (uint8_t *)heap_caps_malloc(MAX_TTS_BYTES, MALLOC_CAP_SPIRAM);
    s_tts_stereo = (uint8_t *)heap_caps_malloc(TTS_STEREO_FRAMES * BYTES_PER_FRAME, MALLOC_CAP_SPIRAM);
    assert(rec && chunk && s_beep_buf && s_tts_buf && s_tts_stereo &&
           "failed to allocate PSRAM buffers");

    s_audio_mux = xSemaphoreCreateMutex();
    assert(s_audio_mux);

    // HTTP server so max's Piper TTS can push voice messages to the speaker.
    if (espwifi_is_connected()) start_play_server();

    // Start the display poll loop (binary semaphore: poke = immediate refresh).
    s_display_poke = xSemaphoreCreateBinary();
    assert(s_display_poke);
    xTaskCreatePinnedToCore(display_poll_task, "display_poll", 6 * 1024, NULL, 3, NULL, 1);

    // Offline capture: scan the SD queue and start the background drain task.
    if (s_sd_ok) {
        s_sd_mux = xSemaphoreCreateMutex();
        assert(s_sd_mux);
        queue_scan_init();
        refresh_ctx_line();  // show any pending-queue count at boot
        xTaskCreatePinnedToCore(drain_task, "drain", 6 * 1024, NULL, 2, NULL, 1);
    }
    // PWR button toggles airplane mode (defer uploads).
    xTaskCreatePinnedToCore(pwr_button_task, "pwr_btn", 3 * 1024, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "Ready. Hold BOOT (GPIO%d) and speak (up to %ds); release to stop.",
             BOOT_BUTTON_PIN, MAX_RECORD_SEC);

    const size_t max_samples = REC_MONO_BYTES / BYTES_PER_SAMPLE;
    for (;;) {
        if (!ptt_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Hold the codec for the whole cycle so an incoming /play can't interleave.
        xSemaphoreTake(s_audio_mux, portMAX_DELAY);
        ESP_LOGI(TAG, "REC start");
        beep_start();

        int16_t *dst = (int16_t *)(rec + WAV_HDR_BYTES);
        size_t mono_samples = 0;
        double peak_rms = 0.0;
        while (ptt_pressed() && mono_samples < max_samples) {
            audio_playback_read(chunk, CHUNK_BYTES);          // interleaved stereo
            const int16_t *src = (const int16_t *)chunk;
            size_t frames = CHUNK_BYTES / BYTES_PER_FRAME;
            for (size_t i = 0; i < frames && mono_samples < max_samples; i++) {
                int32_t l = src[2 * i], r = src[2 * i + 1];
                dst[mono_samples++] = (int16_t)((l + r) / 2);  // downmix to mono
            }
            double rr = rms_int16(chunk, CHUNK_BYTES);
            if (rr > peak_rms) peak_rms = rr;
        }
        size_t mono_bytes = mono_samples * BYTES_PER_SAMPLE;
        ESP_LOGI(TAG, "REC done: %.2f s, %u mono bytes, peak rms=%.0f",
                 (float)mono_bytes / MONO_BYTES_PER_SEC, (unsigned)mono_bytes, peak_rms);
        beep_stop();

        if (mono_bytes < MIN_SEND_MONO) {
            ESP_LOGW(TAG, "  too short (<0.25s) — discarding.");
            beep_fail();
        } else {
            write_wav_header(rec, mono_bytes, SAMPLE_RATE_HZ, 1, 16);
            size_t wav_len = WAV_HDR_BYTES + mono_bytes;
            const char *target = mode_target();   // max | leo | note
            long long epoch = now_epoch();
            bool sent = false;
            if (espwifi_is_connected()) {
                sent = (post_and_show(rec, wav_len, target, epoch) == 200);  // deliver now
            }
            if (sent) {
                beep_ok();
            } else if (s_sd_ok && save_to_queue(rec, wav_len, target, epoch)) {
                beep_queued();   // offline/failed → saved on SD, drains to `target` later
            } else {
                beep_fail();
            }
        }
        xSemaphoreGive(s_audio_mux);

        vTaskDelay(pdMS_TO_TICKS(150));  // debounce release edge
    }
}
