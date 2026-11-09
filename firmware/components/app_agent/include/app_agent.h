/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The panel's voice agent, in the ESP-Claw / ESP-Mosaico shape: the command
 * window's audio goes to an external STT (OpenAI Whisper), the transcript goes
 * to an OpenAI-compatible chat endpoint (NVIDIA-hosted GLM by default) with a
 * capability manifest and tools, and the tool calls — setpoints, status, Lua
 * scripts — execute here. Configured entirely at runtime (NVS, console):
 *   agent-key-stt <key>      the STT bearer key
 *   agent-key-llm <key>      the chat endpoint bearer key
 *   agent-model [id]         show or set the model id
 *   agent-test "<text>"      run the agent on typed text, no audio involved
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How the agent reaches the product. Registered by app_logic at boot so this
 *  component depends on no product code (which keeps the component graph
 *  acyclic: app_audio -> app_agent, app_logic -> both). */
typedef struct {
    /** Set one room. 0 = sent, 1 = offline, 2 = already set, 3 = no such room. */
    int (*set_target)(int room_idx, float celsius);
    int (*room_count)(void);
    int (*find_room)(const char *name);        /* case-insensitive, -1 = none */
    const char *(*room_name)(int idx);
    /** The live capability manifest: rooms, temperatures, targets, link. */
    void (*status_json)(char *buf, size_t len);
    void (*banner)(const char *text);          /* on the LCD, 4.5 s */
    /** Quiet the audio pipeline while the agent works: the TLS handshake is
     *  software crypto on this chip and the AFE otherwise owns the CPU. */
    void (*pause_audio)(bool pause);
} app_agent_ops_t;

void app_agent_set_ops(const app_agent_ops_t *ops);

/** Both keys present: the voice path routes utterances to the agent. */
bool app_agent_configured(void);

/** Transcribe one utterance (16 kHz mono s16le) and run the agent on it.
 *  Blocking — call it from the recogniser's task after the window closes.
 *  Outcomes land on the panel (banners) and in the log. */
esp_err_t app_agent_handle_utterance(const int16_t *pcm, size_t samples);

/** Run the agent on plain text (the agent-test path). Blocking. */
esp_err_t app_agent_handle_text(const char *text);

/** NVS-backed setup + console commands. Call once at boot (after NVS). */
esp_err_t app_agent_init(void);

/* ── settings-page access ────────────────────────────────────────────── */

const char *app_agent_llm_url(void);
const char *app_agent_model(void);
bool app_agent_has_llm_key(void);
bool app_agent_has_stt_key(void);

/** Update any subset of the agent's config; NULL or empty keeps the stored
 *  value. Persisted to NVS and applied to the next request. */
esp_err_t app_agent_config_set(const char *llm_url, const char *model,
                               const char *llm_key, const char *stt_key);

#ifdef __cplusplus
}
#endif
