/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The generic dot product the voice front end is built against.
 *
 * See dsps_dotprod_arp4.c: esp-sr's prebuilt c_speech_features calls
 * `dsps_dotprod_f32_arp4`, and esp-dsp's assembly implementation of that symbol
 * faults on this target. This component provides the same symbol from C.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Which dot-product kernel this image links — "generic C (app_dsp_shim)" when
 *  this component is in the build, which is the safe one. */
const char *app_dsp_dotprod_kernel(void);

#ifdef __cplusplus
}
#endif
