#include "app_driver.h"
#include "app_config.h"

#include <esp_log.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_codec_dev.h>
#include "esp_board_manager.h"
#include "dev_audio_codec.h"
#include "periph_i2s.h"

#include "esp_board_manager.h"
#include "dev_led_strip.h"

static const char *TAG = "app_driver";

/* codec: the board's own 'audio_dac' audio codec — the same
 * square-wave generator as drivers/speaker_es8311, writing into the codec
 * device esp-board-manager already created. No pin, port or address appears
 * in this product: they are the board's. */

static esp_codec_dev_handle_t s_codec_codec = NULL;
static int s_codec_rate = 16000;

/* Written from driver_apply_cases (any task), read by the audio task.
 * Scalar, aligned, single-writer-per-field — volatile is sufficient; a lock
 * here would risk blocking the param bus behind a codec write. A change takes
 * effect on the next chunk boundary. */
static volatile uint32_t s_codec_tone_hz = 0;
static volatile uint8_t  s_codec_volume  = 50;
static volatile bool     s_codec_muted   = false;

static void codec_acodec_task(void *arg)
{
    static int16_t codec_buf[CODEC_ACODEC_CHUNK * CODEC_ACODEC_CHANNELS];
    /* Q16 phase: low 16 bits are the fraction of one cycle. */
    uint32_t phase = 0;

    while (1) {
        uint32_t hz  = s_codec_tone_hz;
        uint8_t  vol = s_codec_volume;
        bool     mut = s_codec_muted;

        if (hz == 0 || mut || vol == 0) {
            memset(codec_buf, 0, sizeof(codec_buf));
            phase = 0;
        } else {
            if (vol > 100) vol = 100;
            /* 12000 of int16 full scale leaves headroom so a class-D amp
             * doesn't clip on the square wave's edges. */
            int16_t amp = (int16_t)((12000 * (int32_t)vol) / 100);
            uint32_t step = (uint32_t)(((uint64_t)hz << 16) / (uint32_t)s_codec_rate);
            for (int i = 0; i < CODEC_ACODEC_CHUNK; i++) {
                int16_t s = (phase & 0x8000u) ? amp : (int16_t)(-amp);
                for (int c = 0; c < CODEC_ACODEC_CHANNELS; c++) {
                    codec_buf[i * CODEC_ACODEC_CHANNELS + c] = s;
                }
                phase = (phase + step) & 0xFFFFu;
            }
        }
        /* Blocks until the codec's DMA takes the chunk — this paces the loop. */
        if (esp_codec_dev_write(s_codec_codec, codec_buf, (int)sizeof(codec_buf)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));   /* never spin on a failing codec */
        }
    }
}

/* The MCLK ratio and sample rate the board's I2S peripheral is configured
 * for. esp_codec_dev 2.x needs the ratio at open time; a mismatch leaves the
 * codec silent with no error anywhere. 0 = the board states none. */
static void codec_board_clock(int *rate, int *mclk_multiple)
{
    *rate = 0;
    *mclk_multiple = 0;
    dev_audio_codec_config_t *dcfg = NULL;
    if (esp_board_manager_get_device_config(CODEC_ACODEC_DEVICE, (void **)&dcfg) != ESP_OK ||
        dcfg == NULL || dcfg->i2s_cfg.name == NULL) {
        return;
    }
    periph_i2s_config_t *pcfg = NULL;
    if (esp_board_manager_get_periph_config(dcfg->i2s_cfg.name, (void **)&pcfg) != ESP_OK || pcfg == NULL) {
        return;
    }
    if (pcfg->mode == I2S_COMM_MODE_STD) {
        *rate = (int)pcfg->i2s_cfg.std.clk_cfg.sample_rate_hz;
        *mclk_multiple = (int)pcfg->i2s_cfg.std.clk_cfg.mclk_multiple;
    }
#if CONFIG_SOC_I2S_SUPPORTS_TDM
    else if (pcfg->mode == I2S_COMM_MODE_TDM) {
        *rate = (int)pcfg->i2s_cfg.tdm.clk_cfg.sample_rate_hz;
        *mclk_multiple = (int)pcfg->i2s_cfg.tdm.clk_cfg.mclk_multiple;
    }
#endif
}

/* status: the board's own 'led_strip' led_strip device.
 * The pin, LED count and RMT/SPI backend are read from the BOARD at init — no
 * GPIO number appears anywhere in this product, which is the point: the line
 * belongs to the board definition and claiming it here would be an IO
 * conflict with it. NULL until init resolves the handle, and every write is a
 * no-op until then, so a missing device degrades to "does nothing".
 *
 * An OBSERVER, not an owner: this block registers a notify callback instead
 * of contributing an apply case, so it can indicate a param another driver
 * owns (a relay's POWER) without emitting a duplicate `case` label in
 * app_driver_apply_param(). */
#define STATUS_COLOUR_MODE 1
static led_strip_handle_t s_status_strip = NULL;
static uint32_t s_status_leds = 0;
static bool s_status_on = false;
static uint8_t s_status_bri = 254;

/* Hue, saturation and brightness are on Matter's 0..254 scales, but the bus
 * carries a plain u8: anything else that sets them (a console command, an MQTT
 * or AWS payload) can send 255. Clamped on the way in — at 255 the brightness
 * scaling below computes 256, which wraps to 0 and turns the LED OFF, and a
 * hue of 255 falls out of the six colour sectors. */
static inline uint8_t status_scale254(uint8_t v) { return v > 254 ? 254 : v; }

#ifdef STATUS_COLOUR_MODE
static uint8_t s_status_hue = 0;
static uint8_t s_status_sat = 254;

/* HSV on Matter's 0..254 scales → 0..255 RGB (six 42.5-step hue sectors). */
static void status_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint16_t h6 = (uint16_t)h * 6U;
    uint16_t region = h6 / 255U;
    uint16_t remainder = (h6 % 255U) * 256U / 255U;
    uint8_t p = ((uint16_t)v * (255U - s)) / 255U;
    uint8_t q = ((uint16_t)v * (255U - (uint16_t)s * remainder / 255U)) / 255U;
    uint8_t t = ((uint16_t)v * (255U - (uint16_t)s * (255U - remainder) / 255U)) / 255U;
    switch (region) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}
#endif

static void status_board_strip_render(void)
{
    if (s_status_strip == NULL) {
        return;
    }
    if (!s_status_on) {
        led_strip_clear(s_status_strip);
        return;
    }
    uint8_t r, g, b;
#ifdef STATUS_COLOUR_MODE
    status_hsv_to_rgb(s_status_hue, s_status_sat, (uint8_t)((uint16_t)s_status_bri * 255U / 254U), &r, &g, &b);
#else
    r = (uint8_t)((uint16_t)255   * s_status_bri / 254U);
    g = (uint8_t)((uint16_t)255 * s_status_bri / 254U);
    b = (uint8_t)((uint16_t)255  * s_status_bri / 254U);
#endif
    for (uint32_t i = 0; i < s_status_leds; i++) {
        led_strip_set_pixel(s_status_strip, i, r, g, b);
    }
    led_strip_refresh(s_status_strip);
}

/* Kept for the shape every other indicator uses: on/off straight through. */
static void status_board_strip_set(bool on)
{
    s_status_on = on;
    status_board_strip_render();
}

static void status_board_strip_notify(
    app_driver_param_id_t param_id, app_driver_param_val_t val,
    app_driver_handle_t source, void *ctx)
{
    (void)source; (void)ctx;
    if (param_id == APP_DRIVER_PARAM_POWER) {
        ESP_LOGI(TAG, "Set status ('led_strip'): %s", val.b ? "ON" : "OFF");
        status_board_strip_set(val.b);
        return;
    }
    if (param_id == APP_DRIVER_PARAM_HUE) { s_status_hue = status_scale254(val.u8); status_board_strip_render(); return; }
#ifdef STATUS_COLOUR_MODE   /* saturation means nothing without a hue */
    if (param_id == APP_DRIVER_PARAM_SATURATION) { s_status_sat = status_scale254(val.u8); status_board_strip_render(); return; }
#endif
    if (param_id == APP_DRIVER_PARAM_BRIGHTNESS) { s_status_bri = status_scale254(val.u8); status_board_strip_render(); return; }
}

static void zc_codec_driver_init(void)
{
    /* esp_board_manager_init() ran in app_main BEFORE app_driver_init(), so the
       codec device 'audio_dac' — its I2S data path, its I2C control and its
       amplifier — already exists. This asks for it; it creates nothing. */
    dev_audio_codec_handles_t *codec_h = NULL;
    if (esp_board_manager_get_device_handle(CODEC_ACODEC_DEVICE, (void **)&codec_h) != ESP_OK ||
        codec_h == NULL || codec_h->codec_dev == NULL) {
        /* Generation refuses a device the board does not declare, so reaching
           here means the board manager failed to bring it up at boot. Degraded,
           not fatal: the product keeps running, it just cannot make a sound. */
        ESP_LOGE(TAG, "codec: board audio codec 'audio_dac' has no handle — audio disabled");
    } else {
        int codec_board_rate = 0, codec_mclk = 0;
        codec_board_clock(&codec_board_rate, &codec_mclk);
        s_codec_rate = CODEC_ACODEC_RATE > 0 ? CODEC_ACODEC_RATE
                             : (codec_board_rate > 0 ? codec_board_rate : 16000);

        esp_codec_dev_sample_info_t codec_fs = {};
        codec_fs.bits_per_sample = 16;
        codec_fs.channel = CODEC_ACODEC_CHANNELS;
        codec_fs.sample_rate = (uint32_t)s_codec_rate;
        if (codec_mclk > 0) {
            codec_fs.mclk_multiple = (uint16_t)codec_mclk;
        }
        if (esp_codec_dev_open(codec_h->codec_dev, &codec_fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codec: codec 'audio_dac' would not open @%d Hz — audio disabled", s_codec_rate);
        } else {
            esp_codec_dev_set_out_vol(codec_h->codec_dev, CODEC_ACODEC_CODEC_VOL);
            s_codec_codec = codec_h->codec_dev;
            xTaskCreate(codec_acodec_task, "codec_acodec", 3072, NULL, 5, NULL);
            ESP_LOGI(TAG, "codec: board audio codec 'audio_dac' @%d Hz, %d ch, mclk x%d",
                     s_codec_rate, CODEC_ACODEC_CHANNELS, codec_mclk);
        }
    }
}

static void zc_status_driver_init(void)
{
        /* esp_board_manager_init() ran in app_main BEFORE app_driver_init(), so
           the led_strip for 'led_strip' already exists — this asks for the
           handle, it does not create anything. The config comes along for the LED
           count: how many pixels the board soldered on is the board's fact. */
        void *status_h = NULL;
        dev_led_strip_config_t *status_cfg = NULL;
        if (esp_board_manager_get_device_handle("led_strip", &status_h) != ESP_OK ||
            esp_board_manager_get_device_config("led_strip", (void **)&status_cfg) != ESP_OK ||
            status_h == NULL || status_cfg == NULL) {
            /* Generation refuses a device the board does not declare, so reaching
               here means the board manager failed to bring it up at boot. Degraded,
               not fatal: the product keeps running without this indicator. */
            ESP_LOGE(TAG, "status: board device 'led_strip' has no handle — indicator disabled");
        } else {
            s_status_strip = ((dev_led_strip_handles_t *)status_h)->strip_handle;
            s_status_leds  = status_cfg->strip_config.max_leds;
            if (s_status_leds == 0) {
                s_status_leds = 1;
            }
            /* BORN OFF — whatever the strip showed at board init, deassert before
               any param can arrive. Same rule as "actuators must be born safe". */
            status_board_strip_set(false);
            /* Observe, don't own: notified on every APP_DRIVER_PARAM_POWER change from
               any other source (button, console, a transport). */
            app_driver_register_solution("status_board_led", status_board_strip_notify, NULL);
            /* A colour already on the bus (restored from NVS, or set before this
               init ran) is not re-announced: read it, or the first power-on shows
               the defaults (full-brightness red) instead. Only values that were
               really set — an unset param reads as zero. */
            app_driver_param_val_t status_v;
    #ifdef STATUS_COLOUR_MODE
            if (app_driver_param_seen(APP_DRIVER_PARAM_HUE) && app_driver_get_param(APP_DRIVER_PARAM_HUE, &status_v) == ESP_OK) s_status_hue = status_scale254(status_v.u8);
    #endif
    #ifdef STATUS_COLOUR_MODE
            if (app_driver_param_seen(APP_DRIVER_PARAM_SATURATION) && app_driver_get_param(APP_DRIVER_PARAM_SATURATION, &status_v) == ESP_OK) s_status_sat = status_scale254(status_v.u8);
    #endif
            if (app_driver_param_seen(APP_DRIVER_PARAM_BRIGHTNESS) && app_driver_get_param(APP_DRIVER_PARAM_BRIGHTNESS, &status_v) == ESP_OK) s_status_bri = status_scale254(status_v.u8);
            (void)status_v;
            ESP_LOGI(TAG, "status: board led_strip 'led_strip' bound (%u LED(s), shows APP_DRIVER_PARAM_POWER)",
                     (unsigned)s_status_leds);
        }
}

esp_err_t app_driver_init(void)
{
    zc_codec_driver_init();
    zc_status_driver_init();

    ESP_LOGI(TAG, "Hardware drivers initialized");
    return ESP_OK;
}

esp_err_t app_driver_apply_param(app_driver_param_id_t id, app_driver_param_val_t val)
{
    switch (id) {
        case APP_DRIVER_PARAM_TONE_HZ: {
            s_codec_tone_hz = val.u32;
            return ESP_OK;
        }

        case APP_DRIVER_PARAM_VOLUME: {
            s_codec_volume = (val.u8 > 100) ? 100 : val.u8;
            return ESP_OK;
        }


        case APP_DRIVER_PARAM_MUTE: {
            s_codec_muted = val.b;
            return ESP_OK;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}
