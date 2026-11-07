#pragma once

#define CODEC_ACODEC_DEVICE      "audio_dac"
#define CODEC_ACODEC_RATE        0
#define CODEC_ACODEC_CHANNELS    1
#define CODEC_ACODEC_CODEC_VOL   70
/* Frames per esp_codec_dev_write: ~16 ms at 16 kHz, ~5 ms at 48 kHz — bounds
 * how fast a tone/mute change takes effect. */
#define CODEC_ACODEC_CHUNK       256

#define VMIC_VMIC_PORT       1
#define VMIC_VMIC_BCLK_GPIO  4
#define VMIC_VMIC_WS_GPIO    5
#define VMIC_VMIC_DIN_GPIO   6
#define VMIC_VMIC_SLOT_RIGHT 0
/* AFE requires 16 kHz mono — not a tunable. */
#define VMIC_VMIC_SAMPLE_RATE 16000

#define CLOCK_TZ          "GMT0BST,M3.5.0/1,M10.5.0"
#define CLOCK_NTP_SERVER  "pool.ntp.org"

#define APP_PRODUCT_NAME        "Korvo-1 Voice Heating Panel"
#define APP_VENDOR_NAME         "ZeroCode AI"
