
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "audio_pipeline.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "board.h"
#include "audio_hal.h"

static const char *TAG = "EXT_MINI";

#define PI_UART_PORT       UART_NUM_0
#define UART_BAUD          921600
#define FRAME_BYTES        1024
#define MIC_SAMPLE_RATE    16000

#define RING_SECONDS       2
#define RING_SAMPLES       (MIC_SAMPLE_RATE * RING_SECONDS)
#define PRETRIGGER_SAMPLES (MIC_SAMPLE_RATE)   // 1 second pre-trigger audio

#define EMA_ALPHA          0.20f
#define TREND_WINDOW       8

// Initial test thresholds only; tune from real recordings later.
#define TRIGGER_DBFS_ON    (-35.0f)
#define TRIGGER_DBFS_OFF   (-42.0f)
#define TRIGGER_DRMS_EMA   (2.5f)
#define TRIGGER_PEAK       6000
#define TRIGGER_TREND      (0.35f)
#define LOCAL_RELEASE_MS   6000   // safety timeout if Pi never sends STOP

#define PKT_EVENT_START    0x01
#define PKT_FEATURE        0x02
#define PKT_PCM            0x03
#define PKT_EVENT_END      0x04
#define PKT_HELLO          0x05

#define MAGIC0 'A'
#define MAGIC1 'M'
#define MAGIC2 'S'
#define MAGIC3 '1'

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t type;
    uint16_t length;
    uint32_t seq;
} packet_header_t;

static audio_board_handle_t g_board = NULL;
static audio_pipeline_handle_t g_recorder_pipeline = NULL;
static audio_element_handle_t g_i2s_read = NULL;
static audio_element_handle_t g_raw_writer = NULL;

static int16_t *g_ring = NULL;
static size_t g_ring_write = 0;
static size_t g_ring_count = 0;

static float g_db_hist[TREND_WINDOW] = {0};
static int g_db_hist_count = 0;
static int g_db_hist_write = 0;

static volatile bool g_trigger_active = false;
static volatile bool g_streaming = false;
static volatile bool g_stop_requested = false;
static TickType_t g_stream_start_tick = 0;
static uint32_t g_seq = 0;

static void ring_write_samples(const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        g_ring[g_ring_write] = samples[i];
        g_ring_write = (g_ring_write + 1) % RING_SAMPLES;
        if (g_ring_count < RING_SAMPLES) {
            g_ring_count++;
        }
    }
}

static float update_trend(float dbfs)
{
    g_db_hist[g_db_hist_write] = dbfs;
    g_db_hist_write = (g_db_hist_write + 1) % TREND_WINDOW;
    if (g_db_hist_count < TREND_WINDOW) g_db_hist_count++;

    if (g_db_hist_count < 2) return 0.0f;

    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_x2 = 0.0f;
    for (int i = 0; i < g_db_hist_count; ++i) {
        int idx = (g_db_hist_write - g_db_hist_count + i + TREND_WINDOW) % TREND_WINDOW;
        float x = (float)i;
        float y = g_db_hist[idx];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    float n = (float)g_db_hist_count;
    float denom = n * sum_x2 - sum_x * sum_x;
    if (fabsf(denom) < 1e-6f) return 0.0f;
    return (n * sum_xy - sum_x * sum_y) / denom;
}

static bool trigger_candidate(float dbfs, float drms_ema, int peak, float trend)
{
    if (dbfs < TRIGGER_DBFS_ON) return false;

    int supporting = 0;
    if (drms_ema >= TRIGGER_DRMS_EMA) supporting++;
    if (peak >= TRIGGER_PEAK) supporting++;
    if (trend >= TRIGGER_TREND) supporting++;
    return supporting >= 1;
}

static esp_err_t send_packet(uint8_t type, const void *payload, uint16_t length)
{
    packet_header_t hdr = {
        .magic = {MAGIC0, MAGIC1, MAGIC2, MAGIC3},
        .type = type,
        .length = length,
        .seq = g_seq++,
    };

    int n = uart_write_bytes(PI_UART_PORT, (const char *)&hdr, sizeof(hdr));
    if (n != sizeof(hdr)) return ESP_FAIL;

    if (length > 0 && payload != NULL) {
        n = uart_write_bytes(PI_UART_PORT, (const char *)payload, length);
        if (n != length) return ESP_FAIL;
    }
    return ESP_OK;
}

static void send_feature(float dbfs, float drms, float ema, int peak, float trend)
{
    char msg[128];
    int len = snprintf(msg, sizeof(msg),
                       "dbfs=%.2f,drms=%.2f,ema=%.2f,peak=%d,trend=%.3f",
                       dbfs, drms, ema, peak, trend);
    if (len > 0) send_packet(PKT_FEATURE, msg, (uint16_t)len);
}

static void send_pretrigger_audio(void)
{
    if (g_ring_count == 0) return;

    size_t want = g_ring_count < PRETRIGGER_SAMPLES ? g_ring_count : PRETRIGGER_SAMPLES;
    size_t start = (g_ring_write + RING_SAMPLES - want) % RING_SAMPLES;

    // Send in small contiguous chunks so wrap-around is handled.
    while (want > 0) {
        size_t contiguous = RING_SAMPLES - start;
        if (contiguous > want) contiguous = want;

        size_t max_samples = 512; // 1024-byte PCM packet
        if (contiguous > max_samples) contiguous = max_samples;

        send_packet(PKT_PCM, &g_ring[start], (uint16_t)(contiguous * sizeof(int16_t)));

        start = (start + contiguous) % RING_SAMPLES;
        want -= contiguous;
    }
}

static void start_stream(float dbfs, float drms, float ema, int peak, float trend)
{
    if (g_streaming) return;

    g_trigger_active = true;
    g_streaming = true;
    g_stop_requested = false;
    g_stream_start_tick = xTaskGetTickCount();

    const char *start = "trigger";
    send_packet(PKT_EVENT_START, start, (uint16_t)strlen(start));
    send_feature(dbfs, drms, ema, peak, trend);

    // The ring buffer already includes the trigger frame.
    send_pretrigger_audio();
}

static void stop_stream(const char *reason)
{
    if (!g_streaming) return;

    send_packet(PKT_EVENT_END, reason, (uint16_t)strlen(reason));
    g_streaming = false;
    g_trigger_active = false;
    g_stop_requested = false;
}

static void command_task(void *arg)
{
    uint8_t buf[32];
    char line[32] = {0};
    size_t pos = 0;

    while (1) {
        int n = uart_read_bytes(PI_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        for (int i = 0; i < n; ++i) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (pos > 0) {
                    line[pos] = '\0';
                    if (strcmp(line, "STOP") == 0) {
                        g_stop_requested = true;
                    }
                    pos = 0;
                }
            } else if (pos < sizeof(line) - 1) {
                line[pos++] = c;
            } else {
                pos = 0;
            }
        }
    }
}

static void audio_read_task(void *arg)
{
    audio_element_handle_t raw_writer_el = (audio_element_handle_t)arg;
    int16_t *mic_buf = (int16_t *)calloc(1, FRAME_BYTES);
    if (!mic_buf) {
        ESP_LOGE(TAG, "mic buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    float prev_dbfs = -96.0f;
    float drms_ema = 0.0f;
    bool ema_initialized = false;
    TickType_t last_feature_tick = 0;

    while (1) {
        int bytes_read = raw_stream_read(raw_writer_el, (char *)mic_buf, FRAME_BYTES);
        if (bytes_read <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        int total_samples = bytes_read / (int)sizeof(int16_t);
        if (total_samples <= 0) continue;

        ring_write_samples(mic_buf, (size_t)total_samples);

        int64_t sum_squares = 0;
        int peak = 0;
        for (int i = 0; i < total_samples; ++i) {
            int32_t v = mic_buf[i];
            int32_t a = (v >= 0) ? v : -v;
            if (a > 32767) a = 32767;
            if (a > peak) peak = (int)a;
            sum_squares += (int64_t)v * (int64_t)v;
        }

        float rms = sqrtf((float)sum_squares / (float)total_samples);
        float dbfs = (rms > 0.0f) ? 20.0f * log10f(rms / 32768.0f) : -96.0f;
        float drms = dbfs - prev_dbfs;
        prev_dbfs = dbfs;

        if (!ema_initialized) {
            drms_ema = drms;
            ema_initialized = true;
        } else {
            drms_ema = EMA_ALPHA * drms + (1.0f - EMA_ALPHA) * drms_ema;
        }

        float trend = update_trend(dbfs);
        TickType_t now = xTaskGetTickCount();

        if (!g_streaming && trigger_candidate(dbfs, drms_ema, peak, trend)) {
            start_stream(dbfs, drms, drms_ema, peak, trend);
            // Do not send this same frame again as live PCM: it is already inside pre-trigger buffer.
            continue;
        }

        if (g_streaming) {
            send_packet(PKT_PCM, mic_buf, (uint16_t)bytes_read);

            if ((now - last_feature_tick) >= pdMS_TO_TICKS(250)) {
                send_feature(dbfs, drms, drms_ema, peak, trend);
                last_feature_tick = now;
            }

            if (g_stop_requested) {
                stop_stream("pi_stop");
            } else if ((now - g_stream_start_tick) >= pdMS_TO_TICKS(LOCAL_RELEASE_MS)) {
                // Safety fallback during bring-up. Pi should normally send STOP.
                stop_stream("timeout");
            }
        }
    }
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Keep UART clean for framed binary traffic.
    esp_log_level_set("*", ESP_LOG_NONE);

    g_ring = (int16_t *)calloc(RING_SAMPLES, sizeof(int16_t));
    if (!g_ring) return;

    g_board = audio_board_init();
    if (!g_board) return;

    audio_hal_ctrl_codec(g_board->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    g_recorder_pipeline = audio_pipeline_init(&pipeline_cfg);
    if (!g_recorder_pipeline) return;

    i2s_stream_cfg_t i2s_read_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(
        I2S_NUM_1,
        MIC_SAMPLE_RATE,
        I2S_DATA_BIT_WIDTH_16BIT,
        AUDIO_STREAM_READER);

    g_i2s_read = i2s_stream_init(&i2s_read_cfg);
    if (!g_i2s_read) return;

    audio_element_info_t i2s_read_info = {
        .sample_rates = MIC_SAMPLE_RATE,
        .channels = 1,
        .bits = 16,
    };
    audio_element_setinfo(g_i2s_read, &i2s_read_info);
    i2s_stream_set_clk(g_i2s_read, MIC_SAMPLE_RATE, 16, 1);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    g_raw_writer = raw_stream_init(&raw_cfg);
    if (!g_raw_writer) return;

    audio_pipeline_register(g_recorder_pipeline, g_i2s_read, "i2s_read");
    audio_pipeline_register(g_recorder_pipeline, g_raw_writer, "raw_write");
    const char *recorder_link[] = {"i2s_read", "raw_write"};
    audio_pipeline_link(g_recorder_pipeline, recorder_link, 2);

    uart_set_baudrate(PI_UART_PORT, UART_BAUD);
    esp_err_t uart_ret = uart_driver_install(
        PI_UART_PORT,
        4096, 0, 0, NULL, 0);
    if (uart_ret != ESP_OK && uart_ret != ESP_ERR_INVALID_STATE) return;

    ESP_ERROR_CHECK(audio_pipeline_run(g_recorder_pipeline));

    const char *hello = "16k_s16le_mono";
    send_packet(PKT_HELLO, hello, (uint16_t)strlen(hello));

    xTaskCreate(command_task, "pi_command", 3072, NULL, 9, NULL);
    xTaskCreate(audio_read_task, "external_audio", 6144, g_raw_writer, 10, NULL);
}
