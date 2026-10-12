/*
 * Audio-Master V8.4.2 - CONTINUOUS MUSIC / AEC / VAD / UART HAZARD
 * For ESP32-S3-Korvo-2, ESP-IDF 5.4.4 / esp-sr 2.5.5.
 * Derived exclusively from proven V8.0 code. Mini firmware and Pi PCM/YAMNet
 * pipeline are UNCHANGED. V8.4 changes S3 playback source only.
 *
 * Continuous 16kHz mono music clip (embedded PCM) -> stereo ES8311 and the
 * exact attenuated digital playback reference -> ESP-SR AEC.
 * Music includes instrumental and singing; AEC can leave residuals and VAD
 * may still report voice from music. Do not claim speech-only recognition.
 *
 * UART0 115200 ASCII newline commands:
 *   HAZARD_ON / HAZARD_OFF  (legacy from Pi, 3-second lease)
 *   LOCAL_VAD_ON / LOCAL_VAD_OFF (enable/disable local VAD-DRIVEN DUCK only)
 *   AUDIO_ON / AUDIO_OFF   (continuous music on / silence)
 *   STATUS                 (one concise state snapshot)
 *
 * AEC reference: 4-second ring of DIGITAL PCM actually written to codec,
 * indexed by approximate V8.0 capture-start marker and measured lag=370.
 * Alignment is still approximate, NOT a hardware echo-reference timestamp.
 * S3 demo only. Not safety-critical/safety-certified.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_aec.h"
#include "esp_vad.h"
#include "driver/uart.h"

#define RATE                 16000
#define CHANNELS             2
#define SAMPLES_PER_FRAME    512
#define PCM_BITS             16
#define MIC_GAIN_DB          24.0f
#define OUTPUT_VOLUME        70
#define REF_RING_SAMPLES      65536u /* 4.096s mono, power-of-two */
#define PREROLL_SAMPLES      8192u
#define ALIGN_LAG_SAMPLES    370
#define FRAME_BUDGET_US      32000
#define VAD_FRAME_MS         20
#define VAD_FRAME_SAMPLES    (RATE * VAD_FRAME_MS / 1000)
#define VOICE_CONFIRM_FRAMES 3       /* 60ms; experimental */
#define QUIET_CONFIRM_FRAMES 25      /* 500ms; experimental */

/* Initial trigger gate derived from prior microphone readings, NOT a validated threshold.
 * RMS 85 PCM ≈ -51.7 dBFS. Two consecutive 20-ms windows are required.
 * Trigger timeout 500 ms; recovery 500 ms. Recalibrate with music/voice tests.
 */
#define TRIGGER_RMS_PCM             85
#define TRIGGER_CONFIRM_WINDOWS    2
#define TRIGGER_TIMEOUT_WINDOWS    25
#define RECOVERY_WINDOWS           25

/* Q15 linear PCM gain: -3dB = 0.708x; -6dB = 0.501x.
 * Hazard overrides at -18dB = 0.126x. Gradual PCM ramp avoids abrupt steps.
 * The test signal is attenuated BEFORE being sent to ES8311, and the AEC
 * reference must use exactly the same digitally attenuated PCM samples.
 */
#define DUCK_GAIN_NORMAL_Q15         32767
#define DUCK_GAIN_TRIGGER_Q15        23197
#define DUCK_GAIN_VOICE_Q15          16422
#define DUCK_GAIN_HAZARD_Q15          4125  /* -18dB = ~0.126x; demo only */

/* UART0 is used by the existing ESP-IDF console on GPIO43/44 on this board.
 * The host must connect via the Korvo-2 USB-UART port, not USB-PWR.
 * Send HAZARD_ON\n every 250-500ms while a hazard persists; a missing heartbeat
 * releases the override after HAZARD_LEASE_US, avoiding indefinite muting. */
#define HAZARD_UART_PORT              UART_NUM_0
#define HAZARD_UART_BAUD              115200
#define HAZARD_UART_RX_BUFFER         2048
#define HAZARD_LEASE_US               (3000000LL)
#define DUCK_GAIN_SLEW_Q15_PER_SAMPLE 16

typedef enum {
    DUCK_NORMAL = 0,
    DUCK_TRIGGERED,
    DUCK_VOICE_ACTIVE,
    DUCK_RECOVERY,
    DUCK_STATE_COUNT
} duck_state_t;

static const char *duck_state_name(duck_state_t state)
{
    switch (state) {
        case DUCK_NORMAL:       return "NORMAL";
        case DUCK_TRIGGERED:    return "TRIGGERED";
        case DUCK_VOICE_ACTIVE: return "VOICE_ACTIVE";
        case DUCK_RECOVERY:     return "RECOVERY";
        default:                return "INVALID";
    }
}

static int duck_target_db(duck_state_t state)
{
    switch (state) {
        case DUCK_TRIGGERED:    return -3;
        case DUCK_VOICE_ACTIVE: return -6;
        case DUCK_RECOVERY:     return 0;
        case DUCK_NORMAL:
        default:                return 0;
    }
}

typedef struct {
    duck_state_t state;
    uint16_t state_windows;
    uint16_t energy_run;
    uint32_t transitions[DUCK_STATE_COUNT];
} duck_sm_t;

static void duck_enter(duck_sm_t *sm, duck_state_t next, uint32_t mic_sample)
{
    if (sm->state == next) return;
    duck_state_t old = sm->state;
    sm->state = next;
    sm->state_windows = 0;
    sm->energy_run = 0;
    sm->transitions[next]++;
    ESP_LOGI("AM_S3", "[AM][S3][LOCAL_STATE] t=%.2f old=%s new=%s target_db=%+d",
             (double)mic_sample / RATE, duck_state_name(old), duck_state_name(next),
             duck_target_db(next));
}

static void duck_update(duck_sm_t *sm, bool energy_event, bool voice_confirmed,
                        uint32_t mic_sample)
{
    if (sm->state_windows < UINT16_MAX) sm->state_windows++;
    if (energy_event) {
        if (sm->energy_run < UINT16_MAX) sm->energy_run++;
    } else {
        sm->energy_run = 0;
    }

    switch (sm->state) {
        case DUCK_NORMAL:
            if (sm->energy_run >= TRIGGER_CONFIRM_WINDOWS) {
                duck_enter(sm, DUCK_TRIGGERED, mic_sample);
            }
            break;
        case DUCK_TRIGGERED:
            if (voice_confirmed) {
                duck_enter(sm, DUCK_VOICE_ACTIVE, mic_sample);
            } else if (sm->state_windows >= TRIGGER_TIMEOUT_WINDOWS) {
                duck_enter(sm, DUCK_RECOVERY, mic_sample);
            }
            break;
        case DUCK_VOICE_ACTIVE:
            if (!voice_confirmed) {
                duck_enter(sm, DUCK_RECOVERY, mic_sample);
            }
            break;
        case DUCK_RECOVERY:
            if (voice_confirmed) {
                duck_enter(sm, DUCK_VOICE_ACTIVE, mic_sample);
            } else if (sm->energy_run >= TRIGGER_CONFIRM_WINDOWS) {
                duck_enter(sm, DUCK_TRIGGERED, mic_sample);
            } else if (sm->state_windows >= RECOVERY_WINDOWS) {
                duck_enter(sm, DUCK_NORMAL, mic_sample);
            }
            break;
        default:
            duck_enter(sm, DUCK_NORMAL, mic_sample);
            break;
    }
}

/* ESP-IDF main/CMakeLists.txt must embed music_16k_mono.pcm as BINARY. */
extern const uint8_t music_16k_mono_pcm_start[] asm("_binary_music_16k_mono_pcm_start");
extern const uint8_t music_16k_mono_pcm_end[]   asm("_binary_music_16k_mono_pcm_end");

static const char *TAG = "AM_S3";
static esp_codec_dev_handle_t g_mic = NULL, g_speaker = NULL;
static aec_handle_t *g_aec = NULL;
static vad_handle_t g_vad_raw = NULL, g_vad_clean = NULL;
static int16_t *g_reference = NULL;   /* 4.096-second mono PSRAM playback reference ring */
static int16_t *g_mic_mono = NULL;
static int16_t *g_ref_mono = NULL;
static int16_t *g_clean = NULL;
static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_mic_written = 0;
static bool g_play_started = false;
static uint32_t g_play_start_capture = 0;
static uint32_t g_ref_samples_ready = 0; /* fully written reference sample count */
static bool g_local_duck_enabled = true;
static bool g_audio_enabled = true;
/* Driven by mic/VAD task, consumed by speaker task. Protect with existing lock. */
static int g_pcm_target_db = 0;
static bool g_hazard_command_on = false;
static int64_t g_hazard_lease_end_us = 0;

static bool hazard_override_active(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&g_lock);
    bool active = g_hazard_command_on && now < g_hazard_lease_end_us;
    if (!active && g_hazard_command_on) g_hazard_command_on = false;
    taskEXIT_CRITICAL(&g_lock);
    return active;
}

static void hazard_set(bool on)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&g_lock);
    bool before = g_hazard_command_on && now < g_hazard_lease_end_us;
    g_hazard_command_on = on;
    g_hazard_lease_end_us = on ? now + HAZARD_LEASE_US : 0;
    taskEXIT_CRITICAL(&g_lock);
    if (before != on) {
        ESP_LOGW(TAG, "[AM][S3][HAZARD] state=%s lease_ms=%d target_db=-18",
                 on ? "ON" : "OFF", (int)(HAZARD_LEASE_US / 1000));
    }
}

static void hazard_uart_receiver_task(void *arg)
{
    (void)arg;
    uint8_t buf[64];
    char cmd[40];
    size_t len = 0;
    ESP_LOGI(TAG, "[AM][S3][UART] listening_baud=%d", HAZARD_UART_BAUD);
    while (true) {
        int n = uart_read_bytes(HAZARD_UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(40));
        for (int i = 0; i < n; ++i) {
            char c = (char)buf[i];
            if (c == '\r' || c == '\n') {
                if (len) {
                    cmd[len] = '\0';
                    if (strcmp(cmd, "HAZARD_ON") == 0) hazard_set(true);
                    else if (strcmp(cmd, "HAZARD_OFF") == 0) hazard_set(false);
                    else if (strcmp(cmd, "LOCAL_VAD_ON") == 0 || strcmp(cmd, "LOCAL_VAD_OFF") == 0) {
                        bool enabled = (strcmp(cmd, "LOCAL_VAD_ON") == 0);
                        taskENTER_CRITICAL(&g_lock);
                        g_local_duck_enabled = enabled;
                        taskEXIT_CRITICAL(&g_lock);
                        ESP_LOGW(TAG, "[AM][S3][MODE] local_vad_duck=%s", enabled ? "ON" : "OFF");
                    } else if (strcmp(cmd, "AUDIO_ON") == 0 || strcmp(cmd, "AUDIO_OFF") == 0) {
                        bool enabled = (strcmp(cmd, "AUDIO_ON") == 0);
                        taskENTER_CRITICAL(&g_lock);
                        g_audio_enabled = enabled;
                        taskEXIT_CRITICAL(&g_lock);
                        ESP_LOGW(TAG, "[AM][S3][MODE] test_audio=%s", enabled ? "ON" : "OFF");
                    } else if (strcmp(cmd, "STATUS") == 0) {
                        bool local, audio;
                        int target;
                        taskENTER_CRITICAL(&g_lock);
                        local = g_local_duck_enabled;
                        audio = g_audio_enabled;
                        target = g_pcm_target_db;
                        taskEXIT_CRITICAL(&g_lock);
                        bool hazard = hazard_override_active();
                        ESP_LOGI(TAG, "[AM][S3][STATUS] local_vad=%s test_audio=%s hazard=%s target_db=%+d",
                                 local ? "ON" : "OFF", audio ? "ON" : "OFF", hazard ? "ON" : "OFF", target);
                    } else ESP_LOGW(TAG, "[AM][S3][UART] ignored_command=%s", cmd);
                    len = 0;
                }
            } else if (c >= 32 && c < 127) {
                if (len < sizeof(cmd) - 1) cmd[len++] = c;
                else len = 0; /* reject overlong line */
            } else {
                len = 0; /* ignore non-ASCII and reset partial command */
            }
        }
    }
}

static bool init_hazard_uart(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = HAZARD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(HAZARD_UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE("V8_UART", "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!uart_is_driver_installed(HAZARD_UART_PORT)) {
        err = uart_driver_install(HAZARD_UART_PORT, HAZARD_UART_RX_BUFFER, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE("V8_UART", "uart_driver_install failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (xTaskCreatePinnedToCore(hazard_uart_receiver_task, "v8_uart_rx", 3584,
                                NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE("V8_UART", "Cannot create UART RX task");
        return false;
    }
    return true;
}


static void set_pcm_target_db(int db)
{
    taskENTER_CRITICAL(&g_lock);
    g_pcm_target_db = db;
    taskEXIT_CRITICAL(&g_lock);
}

static int get_pcm_target_db(void)
{
    taskENTER_CRITICAL(&g_lock);
    int db = g_pcm_target_db;
    taskEXIT_CRITICAL(&g_lock);
    return db;
}

static int32_t gain_q15_for_db(int db)
{
    if (db <= -18) return DUCK_GAIN_HAZARD_Q15;
    if (db <= -6) return DUCK_GAIN_VOICE_Q15;
    if (db <= -3) return DUCK_GAIN_TRIGGER_Q15;
    return DUCK_GAIN_NORMAL_Q15;
}


/* No PHASE or fixed-duration playback in V8.3. */
static void read_mode(bool *local, bool *audio)
{
    taskENTER_CRITICAL(&g_lock);
    *local = g_local_duck_enabled;
    *audio = g_audio_enabled;
    taskEXIT_CRITICAL(&g_lock);
}

static uint32_t get_ref_ready(void)
{
    taskENTER_CRITICAL(&g_lock);
    uint32_t ready = g_ref_samples_ready;
    taskEXIT_CRITICAL(&g_lock);
    return ready;
}

static uint32_t read_mic_count(void)
{
    taskENTER_CRITICAL(&g_lock);
    uint32_t n = g_mic_written;
    taskEXIT_CRITICAL(&g_lock);
    return n;
}

static void set_mic_count(uint32_t n)
{
    taskENTER_CRITICAL(&g_lock);
    g_mic_written = n;
    taskEXIT_CRITICAL(&g_lock);
}

static void set_play_marker(uint32_t marker)
{
    taskENTER_CRITICAL(&g_lock);
    g_play_start_capture = marker;
    g_play_started = true;
    g_ref_samples_ready = 0;
    taskEXIT_CRITICAL(&g_lock);
}

static void read_play_marker(bool *started, uint32_t *marker)
{
    taskENTER_CRITICAL(&g_lock);
    *started = g_play_started;
    *marker = g_play_start_capture;
    taskEXIT_CRITICAL(&g_lock);
}

/* Loop actual embedded music PCM; AUDIO_OFF switches to silence.
 * The AEC reference is the SAME ducked music actually sent to the codec.
 * Publish the reference position AFTER successful speaker write.
 */
static void speaker_task(void *arg)
{
    (void)arg;
    int16_t stereo[SAMPLES_PER_FRAME * CHANNELS];
    TickType_t wait_begin = xTaskGetTickCount();
    while (read_mic_count() < PREROLL_SAMPLES) {
        if (xTaskGetTickCount() - wait_begin > pdMS_TO_TICKS(5000)) {
            ESP_LOGE(TAG, "[AM][S3][ERROR] mic_preroll_timeout");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    uint32_t marker = read_mic_count();
    set_play_marker(marker);
    ESP_LOGI(TAG, "[AM][S3][AUDIO] continuous_start=1 marker=%" PRIu32 " ref_lag_samples=%d signal=MUSIC",
             marker, ALIGN_LAG_SAMPLES);
    const uint8_t *music = music_16k_mono_pcm_start;
    const size_t music_nbytes = (size_t)((uintptr_t)music_16k_mono_pcm_end - (uintptr_t)music_16k_mono_pcm_start);
    if (music_nbytes < 2 || (music_nbytes & 1u)) {
        ESP_LOGE(TAG, "[AM][S3][ERROR] music_pcm_invalid_bytes=%u", (unsigned)music_nbytes);
        vTaskDelete(NULL);
        return;
    }
    const uint32_t music_samples = (uint32_t)(music_nbytes / 2u);
    ESP_LOGI(TAG, "[AM][S3][MUSIC] pcm_samples=%" PRIu32 " clip_seconds=%.2f rate=%d channels=1 looping=ON",
             music_samples, (double)music_samples / RATE, RATE);
    uint32_t music_pos = 0, music_loops = 0;
    int32_t applied_gain_q15 = DUCK_GAIN_NORMAL_Q15;
    uint32_t played_samples = 0;
    uint32_t frame = 0;
    while (true) {
        int target_db = get_pcm_target_db();
        const int32_t target_q15 = gain_q15_for_db(target_db);
        bool local, audio;
        read_mode(&local, &audio);
        (void)local;
        for (int i = 0; i < SAMPLES_PER_FRAME; ++i) {
            if (applied_gain_q15 < target_q15) {
                applied_gain_q15 += DUCK_GAIN_SLEW_Q15_PER_SAMPLE;
                if (applied_gain_q15 > target_q15) applied_gain_q15 = target_q15;
            } else if (applied_gain_q15 > target_q15) {
                applied_gain_q15 -= DUCK_GAIN_SLEW_Q15_PER_SAMPLE;
                if (applied_gain_q15 < target_q15) applied_gain_q15 = target_q15;
            }
            /* Decode little-endian signed PCM. Progress even when AUDIO_OFF
             * so toggling audio does not restart the clip unexpectedly. */
            uint32_t byte_pos = music_pos * 2u;
            uint16_t encoded = (uint16_t)music[byte_pos] | ((uint16_t)music[byte_pos + 1u] << 8);
            int16_t src = (int16_t)encoded;
            music_pos++;
            if (music_pos >= music_samples) {
                music_pos = 0;
                music_loops++;
                ESP_LOGI(TAG, "[AM][S3][MUSIC] loop_completed=%" PRIu32, music_loops);
            }
            int32_t v = audio ? (int32_t)src * applied_gain_q15 / 32767 : 0;
            int16_t out = (int16_t)v;
            g_reference[played_samples & (REF_RING_SAMPLES - 1u)] = out;
            stereo[2*i] = out;
            stereo[2*i + 1] = out;
            ++played_samples;
        }
        int ret = esp_codec_dev_write(g_speaker, stereo, sizeof(stereo));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "[AM][S3][ERROR] speaker_write_failed=%d", ret);
            break;
        }
        taskENTER_CRITICAL(&g_lock);
        g_ref_samples_ready = played_samples;
        taskEXIT_CRITICAL(&g_lock);
        if ((frame % 62u) == 0u) {
            ESP_LOGI(TAG, "[AM][S3][AUDIO] t=%.1fs test_audio=%s target_db=%+d applied_q15=%ld",
                     (double)played_samples / RATE, audio ? "ON" : "OFF", target_db, (long)applied_gain_q15);
        }
        ++frame;
    }
    ESP_LOGE(TAG, "[AM][S3][AUDIO] stopped_after_error=1");
    vTaskDelete(NULL);
}

static void microphone_aec_vad_task(void *arg)
{
    (void)arg;
    int16_t stereo[SAMPLES_PER_FRAME * CHANNELS];
    int16_t vad_raw[VAD_FRAME_SAMPLES];
    int16_t vad_clean[VAD_FRAME_SAMPLES];
    int vad_fill = 0;
    uint32_t samples_done = 0, slow_count = 0;
    int64_t dsp_sum_us = 0, dsp_max_us = 0, vad_sum_us = 0, vad_max_us = 0;
    uint32_t vad_windows = 0;
    duck_sm_t duck_sm = {.state = DUCK_NORMAL};
    bool voice_confirmed = false;
    int voice_run = 0, quiet_run = 0;
    uint64_t raw_squares = 0, clean_squares = 0;
    uint64_t window_clean_energy = 0;
    uint32_t stat_samples = 0, interval_vad_windows = 0, interval_raw_yes = 0, interval_clean_yes = 0;

    ESP_LOGI(TAG, "[AM][S3][AEC] continuous_start=1 frame_samples=512 vad_ms=20");
    for (uint32_t frame = 0; ; ++frame) {
        if (esp_codec_dev_read(g_mic, stereo, sizeof(stereo)) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "[AM][S3][ERROR] mic_read_failed_frame=%" PRIu32, frame);
            break;
        }
        bool marker_ready = false;
        uint32_t marker = 0;
        read_play_marker(&marker_ready, &marker);
        uint32_t ref_ready = get_ref_ready();
        for (int i = 0; i < SAMPLES_PER_FRAME; ++i) {
            int32_t pos = (int32_t)samples_done + i;
            int32_t idx = marker_ready ? pos - (int32_t)marker - ALIGN_LAG_SAMPLES : -1;
            g_mic_mono[i] = stereo[CHANNELS * i];
            g_ref_mono[i] = (idx >= 0 && (uint32_t)idx < ref_ready &&
                             (ref_ready - (uint32_t)idx) < REF_RING_SAMPLES)
                            ? g_reference[(uint32_t)idx & (REF_RING_SAMPLES - 1u)] : 0;
        }
        int64_t t0 = esp_timer_get_time();
        aec_process(g_aec, g_mic_mono, g_ref_mono, g_clean);
        int64_t dsp_us = esp_timer_get_time() - t0;
        dsp_sum_us += dsp_us;
        if (dsp_us > dsp_max_us) dsp_max_us = dsp_us;
        if (dsp_us > FRAME_BUDGET_US) slow_count++;

        for (int i = 0; i < SAMPLES_PER_FRAME; ++i) {
            int32_t raw = g_mic_mono[i], clean = g_clean[i];
            raw_squares += (uint64_t)((int64_t)raw * raw);
            clean_squares += (uint64_t)((int64_t)clean * clean);
            window_clean_energy += (uint64_t)((int64_t)clean * clean);
            stat_samples++;
            vad_raw[vad_fill] = (int16_t)raw;
            vad_clean[vad_fill] = (int16_t)clean;
            vad_fill++;
            if (vad_fill == VAD_FRAME_SAMPLES) {
                int64_t v0 = esp_timer_get_time();
                vad_state_t raw_state = vad_process(g_vad_raw, vad_raw, RATE, VAD_FRAME_MS);
                vad_state_t clean_state = vad_process(g_vad_clean, vad_clean, RATE, VAD_FRAME_MS);
                int64_t vad_us = esp_timer_get_time() - v0;
                vad_sum_us += vad_us;
                if (vad_us > vad_max_us) vad_max_us = vad_us;
                vad_windows++;
                interval_vad_windows++;
                interval_raw_yes += (raw_state == VAD_SPEECH);
                interval_clean_yes += (clean_state == VAD_SPEECH);

                if (clean_state == VAD_SPEECH) {
                    voice_run++;
                    quiet_run = 0;
                    if (!voice_confirmed && voice_run >= VOICE_CONFIRM_FRAMES) {
                        voice_confirmed = true;
                        ESP_LOGI(TAG, "[AM][S3][VAD] confirmed=VOICE t=%.2fs", (double)(samples_done + i + 1) / RATE);
                    }
                } else {
                    quiet_run++;
                    voice_run = 0;
                    if (voice_confirmed && quiet_run >= QUIET_CONFIRM_FRAMES) {
                        voice_confirmed = false;
                        ESP_LOGI(TAG, "[AM][S3][VAD] confirmed=NO_VOICE t=%.2fs", (double)(samples_done + i + 1) / RATE);
                    }
                }

                /* Use integer energy to avoid a logarithm every 20ms.
                 * This is post-AEC RMS, NOT an independent hazard detector.
                 * Calibration starts at ~-51.7 dBFS based on V7.1.1 logs.
                 */
                const uint64_t trigger_energy_threshold =
                    (uint64_t)VAD_FRAME_SAMPLES * TRIGGER_RMS_PCM * TRIGGER_RMS_PCM;
                bool energy_event = (window_clean_energy >= trigger_energy_threshold);
                window_clean_energy = 0;
                duck_update(&duck_sm, energy_event, voice_confirmed,
                            samples_done + i + 1);
                /* Do not change AEC/VAD computation when LOCAL_VAD_OFF;
                 * only decouple its output from physical speaker Ducking. */
                bool local, audio;
                read_mode(&local, &audio);
                (void)audio;
                bool hazard = hazard_override_active();
                int desired_db = hazard ? -18 : (local ? duck_target_db(duck_sm.state) : 0);
                int before_db = get_pcm_target_db();
                if (before_db != desired_db) {
                    ESP_LOGW(TAG, "[AM][S3][DUCK] source=%s from_db=%+d to_db=%+d local_state=%s",
                             hazard ? "HAZARD" : (local ? "LOCAL_VAD" : "LOCAL_DISABLED"),
                             before_db, desired_db, duck_state_name(duck_sm.state));
                    set_pcm_target_db(desired_db);
                }
                vad_fill = 0;
            }
        }
        samples_done += SAMPLES_PER_FRAME;
        set_mic_count(samples_done);
        if (((frame + 1u) % 62u) == 0u) {
            double scale = (double)stat_samples * 32768.0 * 32768.0;
            double raw_db = raw_squares ? 10.0*log10((double)raw_squares / scale) : -120.0;
            double clean_db = clean_squares ? 10.0*log10((double)clean_squares / scale) : -120.0;
            bool local, audio;
            read_mode(&local, &audio);
            ESP_LOGI(TAG,
                "[AM][S3][TELEMETRY] t=%.1fs raw_dbfs=%.1f clean_dbfs=%.1f raw_vad=%" PRIu32 "/%" PRIu32 " clean_vad=%" PRIu32 "/%" PRIu32 " confirmed=%s local_vad=%s test_audio=%s state=%s target_db=%+d aec_avg_us=%lld aec_max_us=%lld slow_frames=%" PRIu32 " vad_avg_us=%lld vad_max_us=%lld",
                (double)samples_done / RATE, raw_db, clean_db,
                interval_raw_yes, interval_vad_windows, interval_clean_yes, interval_vad_windows,
                voice_confirmed ? "VOICE" : "NO_VOICE", local ? "ON" : "OFF", audio ? "ON" : "OFF",
                duck_state_name(duck_sm.state), get_pcm_target_db(),
                (long long)(dsp_sum_us / (frame + 1u)), (long long)dsp_max_us, slow_count,
                (long long)(vad_windows ? vad_sum_us / vad_windows : 0), (long long)vad_max_us);
            stat_samples = 0;
            raw_squares = clean_squares = 0;
            interval_vad_windows = interval_raw_yes = interval_clean_yes = 0;
        }
    }
    ESP_LOGE(TAG, "[AM][S3][ERROR] mic_stream_stopped");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[AM][S3][BOOT] version=8.4.2 mode=MUSIC_CONTINUOUS uart=115200 mini_pi_code=UNCHANGED");
    ESP_LOGW(TAG, "[AM][S3][NOTICE] Music contains vocals. AEC/VAD may false-trigger on playback or residual echo.");
    ESP_LOGI(TAG, "[AM][S3][CONFIG] rate=%d aec_frame=%d vad_ms=%d codec_vol=%d lag=%d",
             RATE, SAMPLES_PER_FRAME, VAD_FRAME_MS, OUTPUT_VOLUME, ALIGN_LAG_SAMPLES);
    ESP_LOGI(TAG, "[AM][S3][CONFIG] commands=HAZARD_ON,HAZARD_OFF,LOCAL_VAD_ON,LOCAL_VAD_OFF,AUDIO_ON,AUDIO_OFF,STATUS");
    ESP_LOGI(TAG, "[AM][S3][CONFIG] duck_trigger_db=-3 duck_voice_db=-6 duck_hazard_db=-18 note=DEMO_TUNING");
    ESP_LOGI(TAG, "[AM][S3][MEM] psram_free=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    g_reference = heap_caps_calloc(REF_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_reference) { ESP_LOGE(TAG, "[AM][S3][ERROR] ref_ring_alloc_failed"); return; }
    aec_config_t conf = {
        .mic_num = 1, .ref_num = 1, .out_num = 1,
        .filter_length = 4, .sample_rate = RATE,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_FD_LOW_COST,
        .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    g_aec = aec_create_from_config(&conf);
    if (!g_aec) { ESP_LOGE(TAG, "AEC creation failed"); return; }
    int chunk = aec_get_chunksize(g_aec);
    ESP_LOGI(TAG, "AEC READY: chunk=%d samples", chunk);
    if (chunk != SAMPLES_PER_FRAME) {
        ESP_LOGE(TAG, "AEC expects different frame size; stop safely"); return;
    }
    g_vad_raw = vad_create_with_param(VAD_MODE_0, RATE, VAD_FRAME_MS, 60, 500);
    g_vad_clean = vad_create_with_param(VAD_MODE_0, RATE, VAD_FRAME_MS, 60, 500);
    if (!g_vad_raw || !g_vad_clean) {
        ESP_LOGE(TAG, "VAD creation failed"); return;
    }
    ESP_LOGI(TAG, "WEBRTC VAD READY: %dms, mode=0, raw+clean parallel", VAD_FRAME_MS);
    g_mic_mono = heap_caps_aligned_alloc(16, chunk*sizeof(int16_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    g_ref_mono = heap_caps_aligned_alloc(16, chunk*sizeof(int16_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    g_clean = heap_caps_aligned_alloc(16, chunk*sizeof(int16_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!g_mic_mono || !g_ref_mono || !g_clean) { ESP_LOGE(TAG, "Aligned audio buffers allocation failed"); return; }

    vTaskDelay(pdMS_TO_TICKS(300));
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "bsp_i2c_init: %s", esp_err_to_name(err)); return; }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "No BSP I2C handle"); return; }
    ESP_LOGI(TAG, "Preliminary probe 0x18: %s", esp_err_to_name(i2c_master_probe(bus, 0x18, 100)));
    esp_err_t mic_probe = ESP_ERR_NOT_FOUND, spk_probe = ESP_ERR_NOT_FOUND;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        mic_probe = i2c_master_probe(bus, 0x40, 100);
        spk_probe = i2c_master_probe(bus, 0x18, 100);
        ESP_LOGI(TAG, "Probe #%d: ES7210=%s ES8311=%s", attempt, esp_err_to_name(mic_probe), esp_err_to_name(spk_probe));
        if (mic_probe == ESP_OK && spk_probe == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (mic_probe != ESP_OK || spk_probe != ESP_OK) { ESP_LOGE(TAG, "Codec probe failed"); return; }
    err = bsp_audio_init(NULL);
    if (err != ESP_OK) { ESP_LOGE(TAG, "bsp_audio_init: %s", esp_err_to_name(err)); return; }
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_codec_dev_sample_info_t info = {.sample_rate=RATE, .channel=CHANNELS, .bits_per_sample=PCM_BITS};
    g_mic = bsp_audio_codec_microphone_init();
    if (!g_mic || esp_codec_dev_open(g_mic, &info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Microphone init failed"); return;
    }
    int rc = esp_codec_dev_set_in_gain(g_mic, MIC_GAIN_DB);
    if (rc != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "MIC gain failed: %d", rc);
    ESP_LOGI(TAG, "MICROPHONE READY");
    vTaskDelay(pdMS_TO_TICKS(100));
    g_speaker = bsp_audio_codec_speaker_init();
    if (!g_speaker || esp_codec_dev_open(g_speaker, &info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker init failed"); return;
    }
    rc = esp_codec_dev_set_out_vol(g_speaker, OUTPUT_VOLUME);
    if (rc != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "Output vol failed: %d", rc);
    ESP_LOGI(TAG, "SPEAKER READY");

    /* Set up UART0 before audio starts. Console and hazard commands share USB-UART;
     * Pi can WRITE HAZARD_ON/OFF and READ IDF log output on the same serial device. */
    if (!init_hazard_uart()) {
        ESP_LOGE(TAG, "UART hazard channel failed; stopping before audio test");
        return;
    }
    ESP_LOGI(TAG, "[AM][S3][READY] playback=MUSIC_CONTINUOUS local_vad=ON test_audio=ON hazard=OFF");

    if (xTaskCreatePinnedToCore(microphone_aec_vad_task, "am_s3_aec_vad", 12288, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create mic AEC/VAD task"); return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (xTaskCreatePinnedToCore(speaker_task, "am_s3_speaker", 6144, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Cannot create playback task"); return;
    }
}
