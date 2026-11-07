/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Generic replacement for esp-dsp's RISC-V assembly dot product.
 *
 * esp-sr's prebuilt voice libraries (c_speech_features, behind the wake word
 * and MultiNet front end) call `dsps_dotprod_f32_arp4` directly — the symbol
 * name is baked into the archive, so esp-dsp's CONFIG_DSP_ANSI switch cannot
 * be used (it links the call out as undefined). On this target the assembly
 * faults at its `esp.lp.setup` hardware-loop instruction: the panel aborts
 * with "Instruction address misaligned" on the first audio frame, before the
 * dashboard, the web page, MQTT or the console ever come up.
 *
 * This component defines that exact symbol, so esp-dsp's assembly member for
 * it is never loaded at all (one definition, one archive member: the linker
 * has nothing to look for). It must therefore be LINKED — see the
 * WHOLE_ARCHIVE and the call in app_audio.
 *
 * The body is esp-dsp's own ANSI implementation, arithmetic unchanged:
 *   float acc = 0; for (i = 0; i < len; i++) acc += src1[i] * src2[i];
 *
 * Only that one kernel is replaced; every other esp-dsp routine (FFT, filters,
 * matrix) keeps its optimized implementation.
 */

#include "app_dsp_shim.h"

#include <esp_err.h>

const char *app_dsp_dotprod_kernel(void)
{
    return "generic C (app_dsp_shim)";
}

/** esp_err_t dsps_dotprod_f32(const float *src1, const float *src2, float *dest, int len) */
esp_err_t dsps_dotprod_f32_arp4(const float *src1, const float *src2, float *dest, int len)
{
    float acc = 0.0f;
    for (int i = 0; i < len; i++) {
        acc += src1[i] * src2[i];
    }
    *dest = acc;
    return ESP_OK;
}
