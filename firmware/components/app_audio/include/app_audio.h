/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ESP ZeroCode AI - Voice (esp-sr) Framework
 *
 * BASE: No-op stub. The generator replaces app_audio.cpp with the real
 * implementation when the product has device-type bindings.
 */

#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

/** Open the one-shot command window from a physical trigger (the front
 *  button): the microphone records for 10 s and MultiNet runs on it, exactly
 *  as it does after the wake word. */
void app_audio_listen_once(void);

/** Feed the embedded test sample (components/app_audio/voice_test.pcm) into
 *  the recogniser in place of the mic, once, with the command window open:
 *  model + grammar exercised with known audio. Console command `voice-test`. */
void app_audio_voice_test(void);

/** The last command window's processed audio (16 kHz mono, AFE output),
 *  recorded for reinterpretation; valid until the next window opens. */
size_t app_audio_window_audio(const int16_t **pcm);

/** Decode a prerecorded buffer against the CURRENTLY registered command
 *  grammar: top phrase id (its confidence, 0..100, in *pct) or -1. Runs on
 *  the recogniser's task only — never while the live window is open. */
int app_audio_decode_buffer(const int16_t *pcm, size_t n, int *pct);

/** Pause/resume feeding the mic into the AFE (the mic keeps draining). Held
 *  around the offline reinterpretation passes to keep the CPU breathable. */
void app_audio_pause_feed(bool pause);

/** Close that window now (a second press, or the 10 s timer). */
void app_audio_listen_stop(void);

esp_err_t app_audio_init(void);

/**
 * Raw PCM source for the AFE feed task, provided by drivers/voice_mic_i2s.
 *
 * Declared here rather than in the mic block so app_audio.cpp has a prototype
 * regardless of instance prefix — a product carries at most one voice mic, so
 * the name is fixed by contract. Blocks that provide it define it in their
 * audio_statics slot.
 *
 * Must block until `samples` int16 samples have been written, and return the
 * count actually produced (< samples signals a read error).
 */
size_t zc_audio_mic_read(int16_t *dest, size_t samples);
