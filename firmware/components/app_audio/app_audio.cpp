#include "app_audio.h"
#include "app_dsp_shim.h"
#include "app_driver.h"
#include "app_config.h"
#include "app_logic.h"
#include "heating.h"

#include <esp_log.h>
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
#include <driver/i2s_std.h>

static const char *TAG = "app_audio";

static app_driver_handle_t s_handle = 0;

static srmodel_list_t *s_models = NULL;
static const esp_afe_sr_iface_t *s_afe = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static const esp_mn_iface_t *s_mn = NULL;
static model_iface_data_t *s_mn_data = NULL;

/* vmic: I2S voice capture — raw PCM source for the AFE feed task. */

static i2s_chan_handle_t s_vmic_vmic_rx = NULL;

/* Scratch for the 32->16 bit narrowing. Sized at AFE's feed chunk, which is
 * queried at runtime; 1024 frames is comfortably above it for 16 kHz mono. */
#define VMIC_VMIC_MAX_CHUNK 1024
static int32_t s_vmic_vmic_raw[VMIC_VMIC_MAX_CHUNK];

/* Fixed-name contract declared in app_audio.h — see this block's block.yml.
 * Blocking read; returns the sample count actually produced. */
size_t zc_audio_mic_read(int16_t *dest, size_t samples)
{
    if (!s_vmic_vmic_rx || dest == NULL) return 0;

    size_t done = 0;
    while (done < samples) {
        size_t want = samples - done;
        if (want > VMIC_VMIC_MAX_CHUNK) want = VMIC_VMIC_MAX_CHUNK;

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_vmic_vmic_rx, s_vmic_vmic_raw,
                                         want * sizeof(int32_t), &bytes_read,
                                         portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "vmic: I2S read failed: %s", esp_err_to_name(err));
            return done;
        }

        size_t got = bytes_read / sizeof(int32_t);
        /* 24-bit parts left-justify into the 32-bit slot: the top 16 bits are
         * the usable signal for both 24- and 32-bit mics. */
        for (size_t i = 0; i < got; i++) {
            dest[done + i] = (int16_t)(s_vmic_vmic_raw[i] >> 16);
        }
        done += got;
        if (got == 0) break;
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
    while (1) {
        /* Provided by drivers/voice_mic_i2s (fixed-name contract, app_audio.h). */
        if (zc_audio_mic_read(buf, chunk * channels) == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
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
            heating_ui_poke();   /* the request just woke the screen */
            ESP_LOGI(TAG, "button press — listening for a command");
            heating_voice_print_vocabulary();
            audio_on_event(1);
            continue;
        }

        if (!listening && res->wakeup_state == WAKENET_DETECTED) {
            listening = true;
            s_mn->clean(s_mn_data);
            heating_ui_poke();   /* the wake word just woke the screen */
            ESP_LOGI(TAG, "wake word detected — listening for a command");
            heating_voice_print_vocabulary();
            audio_on_event(1);
            continue;
        }

        if (!listening) continue;

        esp_mn_state_t st = s_mn->detect(s_mn_data, res->data);
        if (st == ESP_MN_STATE_DETECTING) continue;

        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn = s_mn->get_results(s_mn_data);
            if (mn && mn->num > 0) {
                ESP_LOGI(TAG, "command phrase %d (%.0f%%): \"%s\"", mn->phrase_id[0],
                         mn->prob[0] * 100, mn->string);
                /* The heard words go to the screen with the outcome: feedback
                 * that only reached the serial log helped nobody at the wall. */
                heating_voice_heard(mn->string, (int)(mn->prob[0] * 100));
                /* One press, one request: the window is over as soon as the
                 * phrase is in, whatever opened it. */
                s_btn_window = false;
                audio_apply_command(mn->phrase_id[0]);
            }
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "no command heard — sleeping until next trigger");
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

    /* Capture hardware first — AFE has nothing to read without it. */
    {
        /* ESP-IDF 6 deleted the i2s_port_t enum; i2s_chan_config_t.id is a plain
           int now, so the cast is both unnecessary and a hard compile error. */
        i2s_chan_config_t vmic_chan_cfg =
            I2S_CHANNEL_DEFAULT_CONFIG(VMIC_VMIC_PORT, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&vmic_chan_cfg, NULL, &s_vmic_vmic_rx));

        i2s_std_config_t vmic_std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VMIC_VMIC_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)VMIC_VMIC_BCLK_GPIO,
                .ws   = (gpio_num_t)VMIC_VMIC_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din  = (gpio_num_t)VMIC_VMIC_DIN_GPIO,
                .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
            },
        };
        vmic_std_cfg.slot_cfg.slot_mask =
            VMIC_VMIC_SLOT_RIGHT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;

        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_vmic_vmic_rx, &vmic_std_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(s_vmic_vmic_rx));

        ESP_LOGI(TAG, "Voice mic vmic: I2S%d BCLK=%d WS=%d DIN=%d @%d Hz, %s slot",
                 VMIC_VMIC_PORT, VMIC_VMIC_BCLK_GPIO, VMIC_VMIC_WS_GPIO,
                 VMIC_VMIC_DIN_GPIO, VMIC_VMIC_SAMPLE_RATE,
                 VMIC_VMIC_SLOT_RIGHT ? "right" : "left");
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
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_ERR_NO_MEM;
    }
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
    esp_mn_commands_clear();
    audio_register_commands();

    esp_mn_commands_update();
    s_mn->print_active_speech_commands(s_mn_data);

    xTaskCreate(audio_feed_task, "audio_feed", 4096, NULL, 5, NULL);
    xTaskCreate(audio_detect_task, "audio_detect", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "Voice framework initialized");
    return ESP_OK;
}
