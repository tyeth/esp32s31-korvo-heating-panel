#include "app_audio.h"
#include "app_dsp_shim.h"
#include "app_driver.h"
#include "app_config.h"
#include "app_logic.h"
#include "heating.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_afe_config.h>
#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <model_path.h>
#include <esp_board_manager.h>
#include <esp_codec_dev.h>
#include "dev_audio_codec.h"

static const char *TAG = "app_audio";

static app_driver_handle_t s_handle = 0;

static srmodel_list_t *s_models = NULL;
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static const esp_mn_iface_t *s_mn = NULL;
static model_iface_data_t *s_mn_data = NULL;

/* Voice capture: the board's OWN microphone path.
 *
 * The generated vmic block drove a second I2S master (port 1, std mode,
 * 16 kHz, 32-bit) onto GPIO4/5/6 — but on 'esp32_s31_korvo_1' those pins
 * belong to the ES8389 codec's port-0 TDM bus (GPIO4 is its LRCK, GPIO5 its
 * DAC data, GPIO6 its ADC data). Two masters fought over the wires and the
 * "audio" read was clock garbage: MultiNet transcribed whole sentences as
 * nothing, and the rare 18%-confidence match was noise. The mics are read the
 * way the DAC is written (app_driver): through the board manager's
 * 'audio_adc' esp_codec_dev, at the codec's native 48 kHz / 16-bit / 3 TDM
 * slots, then mic 1 is taken and averaged 3:1 down to the AFE's 16 kHz. */

#define MIC_NATIVE_RATE   48000   /* the codec's TDM bus rate — shared with the DAC */
#define MIC_DECIM         (MIC_NATIVE_RATE / 16000)

static esp_codec_dev_handle_t s_mic_dev = NULL;

/* voice-test: a known-good spoken sample (16 kHz mono 16-bit, embedded at
 * build time) played INTO the recogniser in place of the mic — it exercises
 * AFE + WakeNet + MultiNet exactly as live speech does, with the microphone,
 * the room and the speaker taken out of the question. SIZE_MAX = inactive. */
extern const uint8_t voice_test_pcm_start[] asm("_binary_voice_test_pcm_start");
extern const uint8_t voice_test_pcm_end[]   asm("_binary_voice_test_pcm_end");
static volatile size_t s_test_pos = SIZE_MAX;

/* ── command-window capture, for reinterpretation of the same utterance ──
 *
 * While the window is open, every AFE output frame (16 kHz mono, after
 * NS/AGC) is appended here; heating_voice re-decodes it — whole or in tail
 * snippets, against a swapped grammar — when the live match is not confident
 * enough to act on. 11 s covers the 10 s window. */
#define WIN_MAX_SAMPLES (16000 * 11)
static int16_t *s_win = NULL;
static size_t s_win_len = 0;

size_t app_audio_window_audio(const int16_t **pcm)
{
    if (pcm) *pcm = s_win;
    return s_win ? s_win_len : 0;
}

int app_audio_decode_buffer(const int16_t *pcm, size_t n, int *pct)
{
    if (pct) *pct = 0;
    if (s_mn == NULL || s_mn_data == NULL || pcm == NULL || n == 0) return -1;
    int chunk = s_mn->get_samp_chunksize(s_mn_data);
    static int16_t *tmp = NULL;
    static int tmp_chunk = 0;
    if (tmp == NULL || tmp_chunk < chunk) {
        if (tmp) free(tmp);
        tmp = (int16_t *)heap_caps_malloc(chunk * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        tmp_chunk = chunk;
        if (tmp == NULL) return -1;
    }
    s_mn->clean(s_mn_data);
    int found = -1;
    for (size_t off = 0; off < n; off += (size_t)chunk) {
        size_t m = n - off;
        if (m > (size_t)chunk) m = (size_t)chunk;
        memcpy(tmp, pcm + off, m * sizeof(int16_t));
        if (m < (size_t)chunk) memset(tmp + m, 0, (chunk - m) * sizeof(int16_t));
        esp_mn_state_t st = s_mn->detect(s_mn_data, tmp);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn = s_mn->get_results(s_mn_data);
            if (mn && mn->num > 0) {
                if (pct) *pct = (int)(mn->prob[0] * 100);
                found = mn->phrase_id[0];
            }
            break;
        }
        if (st == ESP_MN_STATE_TIMEOUT) break;
    }
    s_mn->clean(s_mn_data);
    return found;
}

void app_audio_voice_test(void)
{
    const int16_t *pcm = (const int16_t *)voice_test_pcm_start;
    size_t total = (size_t)(voice_test_pcm_end - voice_test_pcm_start) / 2;
    if (total < 16000) {
        ESP_LOGW(TAG, "voice-test: no sample embedded — put a 16 kHz mono 16-bit PCM "
                      "at components/app_audio/voice_test.pcm and rebuild");
        return;
    }

    /* First, the control experiment: the recogniser on the RAW sample, no AFE
     * in the way. A high score here with a low live score convicts the AFE
     * processing; matching low scores convict the model or the content. */
    int pct = 0;
    int id = app_audio_decode_buffer(pcm, total, &pct);
    ESP_LOGI(TAG, "voice-test: RAW decode, no AFE: id %d at %d%%", id, pct);

    ESP_LOGI(TAG, "voice-test: feeding embedded sample (%u samples) instead of the mic",
             (unsigned)total);
    s_test_pos = 0;
    app_audio_listen_once();      /* open the command window, as the button does */
}

/* Scratch for one AFE chunk: samples * 3 (decimation), mono. */
#define VMIC_VMIC_MAX_CHUNK 1024
static int16_t s_mic_raw[VMIC_VMIC_MAX_CHUNK * MIC_DECIM];

/* Fixed-name contract declared in app_audio.h — see this block's block.yml.
 * Blocking read; returns the sample count actually produced (16 kHz mono). */
size_t zc_audio_mic_read(int16_t *dest, size_t samples)
{
    if (dest == NULL) return 0;

    /* An armed voice-test preempts the mic until the sample runs out; paced to
     * real time so the AFE's internal buffering behaves as it does live. */
    if (s_test_pos != SIZE_MAX) {
        const int16_t *pcm = (const int16_t *)voice_test_pcm_start;
        size_t total = (size_t)(voice_test_pcm_end - voice_test_pcm_start) / 2;
        size_t n = 0;
        size_t pos = s_test_pos;
        while (n < samples && pos < total) dest[n++] = pcm[pos++];
        while (n < samples) dest[n++] = 0;
        if (pos >= total) {
            s_test_pos = SIZE_MAX;
            ESP_LOGI(TAG, "voice-test: sample finished — mic restored");
        } else {
            s_test_pos = pos;
        }
        vTaskDelay(pdMS_TO_TICKS(samples / 16));   /* 16 samples per ms */
        return samples;
    }

    if (s_mic_dev == NULL) return 0;

    size_t done = 0;
    while (done < samples) {
        size_t want = samples - done;
        if (want > VMIC_VMIC_MAX_CHUNK) want = VMIC_VMIC_MAX_CHUNK;

        size_t raw_frames = want * MIC_DECIM;               /* 48 kHz frames */
        int err = esp_codec_dev_read(s_mic_dev, s_mic_raw,
                                     raw_frames * sizeof(int16_t));
        if (err != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "vmic: codec read failed: %d", err);
            return done;
        }
        /* A real anti-alias low-pass ahead of the 3:1 rate drop: the first
         * cut averaged each group of 3 (a boxcar whose stopband barely
         * reaches -10 dB), which folded the codec's 8-24 kHz content onto the
         * 4-8 kHz band where the number-suffix consonants live — and the
         * recogniser kept trading "twenty four" for "twenty"/"sixteen".
         * 33-tap Hamming-windowed sinc, 7.2 kHz cutoff at 48 kHz, unity-gain
         * Q15; history carries the convolution across chunk boundaries. */
        static const int16_t k_lp[33] = {
            31, 62, 50, -39, -177, -218, 0, 421, 668, 289, -730, -1655, -1323,
            988, 4773, 8341, 9806, 8341, 4773, 988, -1323, -1655, -730, 289,
            668, 421, 0, -218, -177, -39, 50, 62, 31
        };
        static int16_t s_hist[32] = {0};
        /* ext = the 32 carried samples + this chunk, so every tap window is
         * fully populated. */
        static int16_t s_ext[32 + VMIC_VMIC_MAX_CHUNK * MIC_DECIM];
        memcpy(s_ext, s_hist, sizeof(s_hist));
        memcpy(s_ext + 32, s_mic_raw, raw_frames * sizeof(int16_t));
        for (size_t o = 0; o < want; o++) {
            int32_t acc = 0;
            const int16_t *x = s_ext + o * MIC_DECIM;
            for (int k = 0; k < 33; k++) {
                acc += (int32_t)k_lp[k] * x[k];
            }
            acc >>= 15;
            if (acc > 32767) acc = 32767;
            if (acc < -32768) acc = -32768;
            dest[done + o] = (int16_t)acc;
        }
        memcpy(s_hist, s_ext + raw_frames, sizeof(s_hist));
        done += want;
    }
    return done;
}

/* Recognized phrase -> hardware. Device types own the cases. */
static void audio_apply_command(int phrase_id)
{
    /* The heating panel's vocabulary — registered below in
     * audio_register_commands(), dispatched here. It owns every id it adds, so
     * there is no other case to fall through to. */
    heating_voice_apply_command(phrase_id);
}

/* The product's command vocabulary, added between the framework's clear() and
 * update() so it becomes the model MultiNet recognises. */
static void audio_register_commands(void)
{
    heating_voice_register();
}

/* Listening-state transitions. Behavior blocks hook these via
 * audio_event_cases — `listening` is in scope: 1 = wake word detected,
 * command window open; 0 = window closed (command handled or timed out).
 * The audio analog of matter_event_cases / zigbee_signal_cases. */
static void audio_on_event(int listening)
{
    /* The panel turns this into its amber strip, the "Listening…" banner and
     * the 10 s countdown bar. */
    heating_voice_on_event(listening);
}

/* Hardware/param bus -> voice feedback. Device types may announce state here. */
static void audio_driver_cb(
    app_driver_param_id_t param_id,
    app_driver_param_val_t val,
    app_driver_handle_t source,
    void *ctx)
{
    (void)param_id;
    (void)val;
    (void)source;
    (void)ctx;

}

static void audio_feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    const int channels = s_afe->get_feed_channel_num(s_afe_data);
    int16_t *buf = (int16_t *)heap_caps_malloc(chunk * channels * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "feed buffer alloc failed (%d samples)", chunk * channels);
        vTaskDelete(NULL);
        return;
    }
    int64_t last_meter_us = 0;
    while (1) {
        /* Provided by drivers/voice_mic_i2s (fixed-name contract, app_audio.h). */
        if (zc_audio_mic_read(buf, chunk * channels) == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        /* Level meter, once a second: the first thing to read when nothing is
         * ever recognised. A quiet room shows a peak of a few hundred; speech
         * at arm's length several thousand; a flat 0 means the capture path
         * itself is dead, whatever the recogniser says. */
        static int16_t s_peak = 0;
        for (int i = 0; i < chunk * channels; i++) {
            int16_t v = buf[i] < 0 ? -buf[i] : buf[i];
            if (v > s_peak) s_peak = v;
        }
        int64_t now = esp_timer_get_time();
        if (now - last_meter_us > 1000000) {
            last_meter_us = now;
            ESP_LOGI(TAG, "mic level: peak %d over the last second", (int)s_peak);
            s_peak = 0;
        }
        s_afe->feed(s_afe_data, buf);
    }
}

/* The one-shot window opened by the front button: the same window the wake
 * word opens, without waiting for the wake word. A press requests it; a
 * second press (or the panel's own timer) requests it closed. Both are one
 * flag the detect task honours whichever state it is in. */
static volatile bool s_btn_window = false;   /* a press wants the mic open */

void app_audio_listen_once(void)
{
    s_btn_window = true;
}

void app_audio_listen_stop(void)
{
    s_btn_window = false;
}

static void audio_detect_task(void *arg)
{
    (void)arg;
    /* Wake word gates command recognition: MultiNet only runs in the window
     * after a detected wake word — or after a press of the front button, the
     * spec's push-to-talk trigger. Either way the window is one request long,
     * which is what keeps idle CPU down. */
    bool listening = false;

    while (1) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;

        if (s_btn_window && !listening) {
            listening = true;
            s_mn->clean(s_mn_data);
            s_win_len = 0;       /* a fresh capture for this window */
            heating_ui_poke();   /* the request just woke the screen */
            ESP_LOGI(TAG, "button press — listening for a command");
            heating_voice_print_vocabulary();
            audio_on_event(1);
            continue;
        }

        if (!listening && res->wakeup_state == WAKENET_DETECTED) {
            listening = true;
            s_mn->clean(s_mn_data);
            s_win_len = 0;       /* a fresh capture for this window */
            heating_ui_poke();   /* the wake word just woke the screen */
            ESP_LOGI(TAG, "wake word detected — listening for a command");
            heating_voice_print_vocabulary();
            audio_on_event(1);
            continue;
        }

        if (!listening) continue;

        /* Keep the window's processed audio: a doubtful match is settled by
         * decoding this same capture again (heating_voice reinterpretation). */
        if (s_win != NULL && res->data != NULL && res->data_size > 0) {
            size_t add = (size_t)res->data_size / sizeof(int16_t);
            if (s_win_len + add > WIN_MAX_SAMPLES) add = WIN_MAX_SAMPLES - s_win_len;
            if (add > 0) {
                memcpy(s_win + s_win_len, res->data, add * sizeof(int16_t));
                s_win_len += add;
            }
        }

        esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTING) continue;

        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn = s_mn->get_results(s_mn_data);
            if (mn && mn->num > 0) {
                ESP_LOGI(TAG, "command phrase %d (%.0f%%): \"%s\" raw \"%s\" (input %.0f dB)",
                         mn->phrase_id[0], mn->prob[0] * 100, mn->string, mn->raw_string,
                         res->data_volume);
                for (int c = 1; c < mn->num && c < 3; c++) {
                    ESP_LOGI(TAG, "  candidate %d: phrase %d (%.0f%%)", c,
                             mn->phrase_id[c], mn->prob[c] * 100);
                }
                /* The heard words go to the screen with the outcome: feedback
                 * that only reached the serial log helped nobody at the wall. */
                heating_voice_heard(mn->string, (int)(mn->prob[0] * 100));
                /* One press, one request: the window is over as soon as the
                 * phrase is in, whatever opened it. */
                s_btn_window = false;
                audio_apply_command(mn->phrase_id[0]);
            }
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            /* The window closed with nothing over the threshold. The model
             * still carries what it HEARD (raw_string, no command graph) and
             * its best near-misses: put them in the log and on the screen,
             * so a failed command reads as "heard this" instead of silence. */
            esp_mn_results_t *mn = s_mn->get_results(s_mn_data);
            if (mn && (mn->raw_string[0] || mn->num > 0)) {
                const char *heard = mn->raw_string[0] ? mn->raw_string : mn->string;
                ESP_LOGI(TAG, "no command matched — heard \"%s\" (best candidate %d at %.0f%%)",
                         heard, mn->num > 0 ? mn->phrase_id[0] : -1,
                         mn->num > 0 ? mn->prob[0] * 100 : 0.0);
                heating_voice_heard(heard, mn->num > 0 ? (int)(mn->prob[0] * 100) : 0);
            } else {
                ESP_LOGI(TAG, "no command heard — sleeping until next trigger");
            }
            s_btn_window = false;
            heating_voice_not_understood();
        }
        listening = false;
        audio_on_event(0);
    }
}

esp_err_t app_audio_init(void)
{
    /* The generic DSP kernel the prebuilt front end calls must be linked or the
     * first audio frame traps in esp-dsp's assembly (see app_dsp_shim): name it
     * here so the dependency is real, and say which one this image got. */
    ESP_LOGI(TAG, "DSP dot product: generic kernel linked (%s)",
             app_dsp_dotprod_kernel());

    s_handle = app_driver_register_solution("audio", audio_driver_cb, NULL);

    /* Capture hardware first — AFE has nothing to read without it. The board's
     * own 'audio_adc' (ES8389 on the port-0 TDM bus), opened the way the
     * record_and_play example does it — open, then set_in_gain — never a
     * second I2S master on the codec's pins (see zc_audio_mic_read above).
     * 48 kHz matches the DAC side already running on the same bus clock. */
    {
        void *adc_h = NULL;
        dev_audio_codec_handles_t *codec = NULL;
        if (esp_board_manager_get_device_handle("audio_adc", &adc_h) == ESP_OK && adc_h != NULL) {
            codec = (dev_audio_codec_handles_t *)adc_h;
        }
        if (codec == NULL || codec->codec_dev == NULL) {
            ESP_LOGE(TAG, "board device 'audio_adc' has no handle — voice disabled");
            return ESP_ERR_NOT_FOUND;
        }
        esp_codec_dev_sample_info_t fs = {};
        fs.sample_rate = MIC_NATIVE_RATE;
        fs.channel = 1;
        fs.bits_per_sample = 16;
        if (esp_codec_dev_open(codec->codec_dev, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "mic codec 'audio_adc' would not open @%d Hz — voice disabled",
                     MIC_NATIVE_RATE);
            return ESP_FAIL;
        }
        /* 30 dB: the record example's recording gain; without it the ADC sits
         * at 0 dB and a command from across the room reaches the recogniser
         * as a whisper. The AFE's AGC rides on top. */
        if (esp_codec_dev_set_in_gain(codec->codec_dev, 30.0f) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mic in-gain not set — recognition range will suffer");
        }
        s_mic_dev = codec->codec_dev;
        ESP_LOGI(TAG, "Voice mic: board 'audio_adc' @%d Hz mono 16-bit, 30 dB in-gain, "
                      "decimated %d:1 for the AFE", MIC_NATIVE_RATE, MIC_DECIM);
    }

    /* Models live in the `model` partition; see the 8mb-voice table. A build
     * with no model partition reaches here and fails, which is the intended
     * loud failure rather than a silent mis-detection. */
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num <= 0) {
        ESP_LOGE(TAG, "no esp-sr models found — is the 'model' partition flashed?");
        return ESP_ERR_NOT_FOUND;
    }

    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!wn_name || !mn_name) {
        ESP_LOGE(TAG, "missing model (wakenet=%s multinet=%s)",
                 wn_name ? wn_name : "none", mn_name ? mn_name : "none");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "models: wakenet=%s multinet=%s", wn_name, mn_name);

    /* "M" = one mic channel, no reference channel (no AEC — nothing plays back
     * into this mic in the reference products). */
    /* HIGH_PERF: better noise/echo cleanup ahead of the recogniser. The
     * low-cost mode left real commands under the detection threshold at
     * living-room distance; this panel has PSRAM to spare for it. */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_ERR_NO_MEM;
    }
    /* Noise suppression and automatic gain ahead of the recogniser: with one
     * economy mic, level and noise are exactly what it is short of. WEBRTC
     * AGC, deliberately: the WAKENET mode only computes a gain when the wake
     * word fires, so every button-opened window (the panel's push-to-talk,
     * and voice-test) ran with NO gain control at all. */
    afe_cfg->ns_init = true;
    afe_cfg->agc_init = true;
    afe_cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
    s_afe = esp_afe_handle_from_config(afe_cfg);
    s_afe_data = s_afe->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_ERR_NO_MEM;
    }

    /* Command window after the wake word — matches the panel's 10 s listening
     * bar (LISTEN_MS): at the old 6000 ms the recogniser gave up four seconds
     * before the bar emptied, and a longer phrase spoken late was never heard. */
    s_mn = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_mn->create(mn_name, 10000);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed");
        return ESP_ERR_NO_MEM;
    }

    /* Phrases are registered at runtime, not from sdkconfig: the vocabulary is
     * the product's (heating_voice_register, in app_logic). */
    /* The reinterpretation capture — in PSRAM, one window's worth. */
    s_win = (int16_t *)heap_caps_malloc(WIN_MAX_SAMPLES * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_win == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the window capture — doubtful commands will "
                      "be rejected instead of re-decoded");
    }

    esp_mn_commands_clear();
    audio_register_commands();

    esp_mn_commands_update();
    /* The default accept threshold let an 18%-confidence guess fire a command
     * ("set bathroom to 24" acted as "stop"). 0.45 keeps junk out; a genuine
     * phrase that lands under it now shows up as 'Heard: "…"' on the timeout
     * path, which is the evidence to tune this further either way. */
    if (s_mn->set_det_threshold) {
        /* Calibrated on this board with an embedded known-good sample: mn7_en
         * scores a CORRECT long phrase at only 11-16%, so anything like the
         * conventional 0.5 silences the panel completely. The model gate stays
         * low; the real acceptance test is per phrase CLASS in
         * heating_voice_apply_command (short phrases score high on noise, long
         * ones score low even when right — one number cannot split them). */
        s_mn->set_det_threshold(s_mn_data, 0.05f);
        ESP_LOGI(TAG, "command accept threshold set to 0.05 (class gate in the product)");
    }
    s_mn->print_active_speech_commands(s_mn_data);

    xTaskCreate(audio_feed_task, "audio_feed", 4096, NULL, 5, NULL);
    xTaskCreate(audio_detect_task, "audio_detect", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Voice framework initialized");
    return ESP_OK;
}
