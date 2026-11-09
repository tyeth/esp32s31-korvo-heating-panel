/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The voice agent (see app_agent.h). Everything here runs blocking on the
 * caller's task — the recogniser's task for voice, the console task for
 * agent-test — and talks HTTPS through the certificate bundle.
 */

#include "app_agent.h"
#include "agent_lua.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/idf_additions.h>
#include <esp_log.h>
#include <esp_console.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <nvs.h>
#include <cJSON.h>
#include <lwip/dns.h>

static const char *TAG = "app_agent";

#define AGENT_NVS_NS       "agent"
#define AGENT_DEF_STT_URL  "https://api.openai.com/v1/audio/transcriptions"
#define AGENT_DEF_LLM_URL  "https://integrate.api.nvidia.com/v1/chat/completions"
#define AGENT_DEF_MODEL    "zai/glm-5.3-flash"
#define AGENT_MAX_TURNS    5
/* Generous on purpose: software RSA chain verification on this preview core
 * (hardware MPI hangs, so it is disabled) takes tens of seconds; the one
 * endpoint with a small ECDSA chain (1.1.1.1) handshook long ago while the
 * RSA-chained APIs kept "timing out". Measure first, optimise after. */
#define HTTP_TIMEOUT_MS    150000

static const app_agent_ops_t *s_ops = NULL;
static char s_stt_key[200] = {0};
static char s_llm_key[200] = {0};
static char s_model[96]    = AGENT_DEF_MODEL;
static char s_llm_url[160] = AGENT_DEF_LLM_URL;
static char s_stt_url[160] = AGENT_DEF_STT_URL;

void app_agent_set_ops(const app_agent_ops_t *ops)
{
    s_ops = ops;
}

bool app_agent_configured(void)
{
    return s_ops != NULL && s_stt_key[0] != 0 && s_llm_key[0] != 0;
}

/* ── HTTPS POST, response into PSRAM ─────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_acc_t;

static esp_err_t http_evt(esp_http_client_event_t *evt)
{
    http_acc_t *acc = (http_acc_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && acc != NULL && evt->data_len > 0) {
        if (acc->len + (size_t)evt->data_len + 1 > acc->cap) {
            size_t ncap = (acc->cap ? acc->cap * 2 : 8192);
            while (ncap < acc->len + (size_t)evt->data_len + 1) ncap *= 2;
            char *nb = (char *)heap_caps_realloc(acc->buf, ncap,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb == NULL) return ESP_ERR_NO_MEM;
            acc->buf = nb;
            acc->cap = ncap;
        }
        memcpy(acc->buf + acc->len, evt->data, (size_t)evt->data_len);
        acc->len += (size_t)evt->data_len;
        acc->buf[acc->len] = '\0';
    }
    return ESP_OK;
}

/** POST body to url with a bearer key. esp_http_client, as intended; the
 *  response accumulates in PSRAM (caller frees), *status gets the HTTP code. */
static char *http_post_once(const char *url, const char *bearer, const char *content_type,
                            const char *body, size_t body_len, int *status)
{
    http_acc_t acc = {};
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_evt;
    cfg.user_data = &acc;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) return NULL;
    char auth[232];
    snprintf(auth, sizeof(auth), "Bearer %s", bearer);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_header(h, "Content-Type", content_type);
    esp_http_client_set_post_field(h, body, (int)body_len);
    esp_err_t err = esp_http_client_perform(h);
    if (status) *status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
        free(acc.buf);
        return NULL;
    }
    return acc.buf;              /* may be NULL for an empty body */
}

/** http_post_once with up to three tries: lwIP's resolver fails transiently
 *  (getaddrinfo 202) when the stack is busy, and one utterance should not
 *  die on a stale DNS moment. */
static char *http_post(const char *url, const char *bearer, const char *content_type,
                       const char *body, size_t body_len, int *status)
{
    /* Re-asserted per request, not once at boot: DHCP rewrites lwIP's DNS
     * slots on every (re)association, which silently removed the fallback —
     * and the LAN's own resolver answers local names while failing public
     * ones for this client (getaddrinfo 202 on example.com, of all things). */
    ip_addr_t dns2 = IPADDR4_INIT_BYTES(8, 8, 8, 8);
    dns_setserver(1, &dns2);

    for (int attempt = 0; ; attempt++) {
        int st = 0;
        char *resp = http_post_once(url, bearer, content_type, body, body_len, &st);
        if (status) *status = st;
        if (resp != NULL || st != 0 || attempt >= 2) return resp;
        ESP_LOGW(TAG, "retrying (%d/2)", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}

/* ── Whisper: one utterance to text ──────────────────────────────────── */

static void wav_header(uint8_t *h, uint32_t pcm_bytes)
{
    uint32_t file = pcm_bytes + 36, rate = 16000, byterate = 32000;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &file, 4); memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16; memcpy(h + 16, &fmtlen, 4);
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 20, &pcm, 2);  memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &byterate, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &pcm_bytes, 4);
}

/** malloc'd transcript (caller frees) or NULL. */
static char *stt_transcribe(const int16_t *pcm, size_t samples)
{
    static const char kBoundary[] = "zcAgentForm7f3a9";
    /* A domain prompt steers Whisper toward the room names it will hear. */
    char bias[200];
    size_t bw = (size_t)snprintf(bias, sizeof(bias), "Smart heating command. Rooms:");
    for (int i = 0; s_ops != NULL && i < s_ops->room_count() && bw + 4 < sizeof(bias); i++) {
        bw += (size_t)snprintf(bias + bw, sizeof(bias) - bw, "%s %s",
                               i ? "," : "", s_ops->room_name(i));
    }
    char head[640], tail[64];
    int head_n = snprintf(head, sizeof(head),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\nen\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", kBoundary, kBoundary, kBoundary, bias, kBoundary);
    int tail_n = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", kBoundary);

    size_t pcm_bytes = samples * sizeof(int16_t);
    size_t total = (size_t)head_n + 44 + pcm_bytes + (size_t)tail_n;
    uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) return NULL;
    memcpy(body, head, (size_t)head_n);
    wav_header(body + head_n, (uint32_t)pcm_bytes);
    memcpy(body + head_n + 44, pcm, pcm_bytes);
    memcpy(body + head_n + 44 + pcm_bytes, tail, (size_t)tail_n);

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", kBoundary);
    int status = 0;
    char *resp = http_post(s_stt_url, s_stt_key, ctype, (const char *)body, total, &status);
    free(body);
    if (resp == NULL || status != 200) {
        ESP_LOGW(TAG, "STT HTTP %d: %.200s", status, resp ? resp : "(no body)");
        free(resp);
        return NULL;
    }
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (j == NULL) return NULL;
    cJSON *t = cJSON_GetObjectItem(j, "text");
    char *out = (t && cJSON_IsString(t)) ? strdup(t->valuestring) : NULL;
    cJSON_Delete(j);
    return out;
}

/* ── the tool set ────────────────────────────────────────────────────── */

static const char *k_tools_json =
"[{\"type\":\"function\",\"function\":{\"name\":\"set_temperature\","
  "\"description\":\"Set one room's heating setpoint in degrees C\","
  "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"room\":{\"type\":\"string\",\"description\":\"room name\"},"
    "\"celsius\":{\"type\":\"number\"}},\"required\":[\"room\",\"celsius\"]}}},"
 "{\"type\":\"function\",\"function\":{\"name\":\"set_all\","
  "\"description\":\"Set every room's heating setpoint in degrees C\","
  "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"celsius\":{\"type\":\"number\"}},\"required\":[\"celsius\"]}}},"
 "{\"type\":\"function\",\"function\":{\"name\":\"get_status\","
  "\"description\":\"Current rooms, temperatures, targets and link state\","
  "\"parameters\":{\"type\":\"object\",\"properties\":{}}}},"
 "{\"type\":\"function\",\"function\":{\"name\":\"run_lua\","
  "\"description\":\"Run a short Lua 5.4 script on the panel. API: "
    "heating.set(room_name_or_index, celsius) -> ok, msg; "
    "heating.rooms() -> array of {name, index}; log(text). "
    "No io/os; 400k instruction budget.\","
  "\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"script\":{\"type\":\"string\"}},\"required\":[\"script\"]}}}]";

/** Executes one tool call; returns a malloc'd result string (caller frees). */
static char *tool_dispatch(const char *name, cJSON *args)
{
    char *out = (char *)malloc(1024);
    if (out == NULL) return NULL;
    out[0] = '\0';

    if (strcmp(name, "get_status") == 0) {
        s_ops->status_json(out, 1024);
        return out;
    }
    if (strcmp(name, "set_temperature") == 0) {
        cJSON *room = cJSON_GetObjectItem(args, "room");
        cJSON *cels = cJSON_GetObjectItem(args, "celsius");
        if (!cJSON_IsString(room) || !cJSON_IsNumber(cels)) {
            snprintf(out, 1024, "error: room (string) and celsius (number) required");
            return out;
        }
        int idx = s_ops->find_room(room->valuestring);
        if (idx < 0) {
            snprintf(out, 1024, "error: no room called '%s'", room->valuestring);
            return out;
        }
        int rc = s_ops->set_target(idx, (float)cels->valuedouble);
        snprintf(out, 1024, "%s -> %.1fC: %s", s_ops->room_name(idx), cels->valuedouble,
                 rc == 0 ? "sent to the TRV" : rc == 2 ? "already at that target"
                                             : rc == 1 ? "panel offline — not sent" : "refused");
        return out;
    }
    if (strcmp(name, "set_all") == 0) {
        cJSON *cels = cJSON_GetObjectItem(args, "celsius");
        if (!cJSON_IsNumber(cels)) {
            snprintf(out, 1024, "error: celsius (number) required");
            return out;
        }
        size_t w = 0;
        for (int i = 0; i < s_ops->room_count(); i++) {
            int rc = s_ops->set_target(i, (float)cels->valuedouble);
            w += (size_t)snprintf(out + w, 1024 - w, "%s%s: %s", i ? "; " : "",
                                  s_ops->room_name(i), rc == 0 ? "sent" :
                                  rc == 2 ? "already set" : rc == 1 ? "offline" : "refused");
            if (w >= 1000) break;
        }
        return out;
    }
    if (strcmp(name, "run_lua") == 0) {
        cJSON *script = cJSON_GetObjectItem(args, "script");
        if (!cJSON_IsString(script)) {
            snprintf(out, 1024, "error: script (string) required");
            return out;
        }
        ESP_LOGI(TAG, "run_lua:\n%s", script->valuestring);
        agent_lua_run(s_ops, script->valuestring, out, 1024);
        if (out[0] == '\0') snprintf(out, 1024, "ok (no output)");
        return out;
    }
    snprintf(out, 1024, "error: unknown tool '%s'", name);
    return out;
}

/* ── the agent loop ──────────────────────────────────────────────────── */

static void system_prompt(char *buf, size_t len)
{
    char state[768];
    s_ops->status_json(state, sizeof(state));
    snprintf(buf, len,
        "You are the voice agent of a wall-mounted heating panel (ESP32). "
        "You control real radiator valves over zigbee2mqtt via the tools. "
        "Rules: setpoints are 5..30 C in 0.5 steps; stay within 15..25 unless "
        "the user explicitly asks beyond. Use run_lua for anything multi-step "
        "or conditional. Confirm what you DID in one short sentence (it is "
        "shown on a small screen); if the request is not about heating, answer "
        "briefly and do nothing. Current device state: %s", state);
}

static esp_err_t agent_run(const char *user_text)
{
    if (!app_agent_configured()) return ESP_ERR_INVALID_STATE;

    char *sys = (char *)malloc(1536);
    if (sys == NULL) return ESP_ERR_NO_MEM;
    system_prompt(sys, 1536);

    cJSON *messages = cJSON_CreateArray();
    {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", sys);
        cJSON_AddItemToArray(messages, m);
        m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON_AddStringToObject(m, "content", user_text);
        cJSON_AddItemToArray(messages, m);
    }
    free(sys);

    esp_err_t result = ESP_FAIL;
    for (int turn = 0; turn < AGENT_MAX_TURNS; turn++) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", s_model);
        cJSON_AddItemToObject(req, "messages", cJSON_Duplicate(messages, 1));
        cJSON *tools = cJSON_Parse(k_tools_json);
        cJSON_AddItemToObject(req, "tools", tools);
        cJSON_AddNumberToObject(req, "temperature", 0.2);
        cJSON_AddNumberToObject(req, "max_tokens", 700);
        char *body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (body == NULL) break;

        int status = 0;
        char *resp = http_post(s_llm_url, s_llm_key, "application/json",
                               body, strlen(body), &status);
        free(body);
        if (resp == NULL || status != 200) {
            ESP_LOGW(TAG, "LLM HTTP %d: %.300s", status, resp ? resp : "(no body)");
            if (s_ops) s_ops->banner("Agent: service error");
            free(resp);
            break;
        }

        cJSON *j = cJSON_Parse(resp);
        free(resp);
        cJSON *msg = j ? cJSON_GetObjectItem(
                             cJSON_GetArrayItem(cJSON_GetObjectItem(j, "choices"), 0),
                             "message")
                       : NULL;
        if (msg == NULL) {
            ESP_LOGW(TAG, "LLM: no message in response");
            cJSON_Delete(j);
            break;
        }

        cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            /* Keep the assistant turn, run every tool, append the results. */
            cJSON_AddItemToArray(messages, cJSON_Duplicate(msg, 1));
            cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, tool_calls) {
                cJSON *fn   = cJSON_GetObjectItem(tc, "function");
                cJSON *id   = cJSON_GetObjectItem(tc, "id");
                cJSON *name = fn ? cJSON_GetObjectItem(fn, "name") : NULL;
                cJSON *argj = fn ? cJSON_GetObjectItem(fn, "arguments") : NULL;
                if (!cJSON_IsString(name)) continue;
                cJSON *args = cJSON_Parse(cJSON_IsString(argj) ? argj->valuestring : "{}");
                ESP_LOGI(TAG, "tool %s(%s)", name->valuestring,
                         cJSON_IsString(argj) ? argj->valuestring : "{}");
                char *tr = tool_dispatch(name->valuestring, args);
                cJSON_Delete(args);
                ESP_LOGI(TAG, "  -> %s", tr ? tr : "(null)");
                cJSON *rm = cJSON_CreateObject();
                cJSON_AddStringToObject(rm, "role", "tool");
                if (cJSON_IsString(id)) {
                    cJSON_AddStringToObject(rm, "tool_call_id", id->valuestring);
                }
                cJSON_AddStringToObject(rm, "content", tr ? tr : "error");
                free(tr);
                cJSON_AddItemToArray(messages, rm);
            }
            cJSON_Delete(j);
            continue;                              /* next turn with results */
        }

        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (cJSON_IsString(content) && content->valuestring[0]) {
            ESP_LOGI(TAG, "agent: %s", content->valuestring);
            if (s_ops) {
                char banner[150];
                snprintf(banner, sizeof(banner), "%s", content->valuestring);
                s_ops->banner(banner);
            }
            result = ESP_OK;
        }
        cJSON_Delete(j);
        break;
    }
    cJSON_Delete(messages);
    return result;
}

/* ── the worker task and its queue ────────────────────────────────────
 *
 * Everything network-y runs HERE, on a stack sized for a TLS handshake plus
 * cJSON plus the Lua sandbox. The first integration ran it all on the console
 * task's 4 KB stack, which the handshake overflowed and the panel rebooted
 * mid-command. Callers only enqueue. */

typedef struct {
    char    *text;      /* owned; NULL when pcm carries the request */
    int16_t *pcm;       /* owned (PSRAM copy of the window) */
    size_t   samples;
} agent_req_t;

static QueueHandle_t s_req_q = NULL;

static void agent_process(agent_req_t *req)
{
    /* The gauge that settles network-vs-memory arguments: lwIP can only SEND
     * if internal RAM has pbufs to give. */
    ESP_LOGI(TAG, "internal heap: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    char *text = req->text;
    if (req->pcm != NULL) {
        if (s_ops) s_ops->banner("...");
        uint32_t ms0 = esp_log_timestamp();
        text = stt_transcribe(req->pcm, req->samples);
        free(req->pcm);
        if (text == NULL) {
            if (s_ops) s_ops->banner("Agent: transcription failed");
            return;
        }
        ESP_LOGI(TAG, "transcript (%u ms): \"%s\"",
                 (unsigned)(esp_log_timestamp() - ms0), text);
        if (s_ops) {
            char heard[150];
            snprintf(heard, sizeof(heard), "Heard: \"%s\"", text);
            s_ops->banner(heard);
        }
    }
    if (text != NULL) {
        ESP_LOGI(TAG, "user: %s", text);
        agent_run(text);
        free(text);
    }
}

static void agent_task(void *arg)
{
    (void)arg;
    agent_req_t req;
    for (;;) {
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) == pdTRUE) {
            /* The whole request runs with the AFE quiet: a TLS handshake is
             * seconds of software bignum on this chip, and with the audio
             * pipeline running it starved past the HTTP timeout ("handshake
             * in progress" for 13+ s, then abort). The wake word is deaf for
             * the few seconds the agent is thinking; the banner shows why. */
            if (s_ops && s_ops->pause_audio) s_ops->pause_audio(true);
            agent_process(&req);
            if (s_ops && s_ops->pause_audio) s_ops->pause_audio(false);
        }
    }
}

/* ── public entry points (enqueue only) ──────────────────────────────── */

esp_err_t app_agent_handle_text(const char *text)
{
    if (text == NULL || text[0] == 0) return ESP_ERR_INVALID_ARG;
    if (s_req_q == NULL) return ESP_ERR_INVALID_STATE;
    agent_req_t req = { strdup(text), NULL, 0 };
    if (req.text == NULL) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        free(req.text);
        return ESP_ERR_TIMEOUT;                /* one request at a time */
    }
    return ESP_OK;
}

esp_err_t app_agent_handle_utterance(const int16_t *pcm, size_t samples)
{
    if (!app_agent_configured() || s_req_q == NULL) return ESP_ERR_INVALID_STATE;
    if (pcm == NULL || samples < 8000) return ESP_ERR_INVALID_ARG;
    /* Our own copy: the capture buffer is the audio task's and the next
     * window overwrites it while this request is still in flight. */
    int16_t *dup = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dup == NULL) return ESP_ERR_NO_MEM;
    memcpy(dup, pcm, samples * sizeof(int16_t));
    agent_req_t req = { NULL, dup, samples };
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        free(dup);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/* ── configuration: NVS + console ────────────────────────────────────── */

static void save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(AGENT_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int cmd_key_stt(int argc, char **argv)
{
    if (argc < 2) { printf("usage: agent-key-stt <bearer key>\n"); return 1; }
    save_str("stt_key", argv[1]);
    snprintf(s_stt_key, sizeof(s_stt_key), "%s", argv[1]);
    printf("stt key stored (%u chars)\n", (unsigned)strlen(argv[1]));
    return 0;
}

static int cmd_key_llm(int argc, char **argv)
{
    if (argc < 2) { printf("usage: agent-key-llm <bearer key>\n"); return 1; }
    save_str("llm_key", argv[1]);
    snprintf(s_llm_key, sizeof(s_llm_key), "%s", argv[1]);
    printf("llm key stored (%u chars)\n", (unsigned)strlen(argv[1]));
    return 0;
}

static int cmd_model(int argc, char **argv)
{
    if (argc < 2) {
        printf("model: %s\nllm:   %s\nstt:   %s\nconfigured: %s\n",
               s_model, s_llm_url, s_stt_url, app_agent_configured() ? "yes" : "no");
        return 0;
    }
    save_str("model", argv[1]);
    snprintf(s_model, sizeof(s_model), "%s", argv[1]);
    printf("model set: %s\n", s_model);
    return 0;
}

static int cmd_url(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: agent-url <chat completions url>\n       current: %s\n", s_llm_url);
        return 1;
    }
    if (strlen(argv[1]) >= sizeof(s_llm_url)) { printf("too long\n"); return 1; }
    save_str("llm_url", argv[1]);
    snprintf(s_llm_url, sizeof(s_llm_url), "%s", argv[1]);
    printf("llm url set: %s\n", s_llm_url);
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    if (argc < 2) { printf("usage: agent-test \"<request>\"\n"); return 1; }
    /* The console splits on spaces; stitch the words back together. */
    char text[256] = {0};
    size_t w = 0;
    for (int i = 1; i < argc && w + 2 < sizeof(text); i++) {
        w += (size_t)snprintf(text + w, sizeof(text) - w, "%s%s", i > 1 ? " " : "", argv[i]);
    }
    esp_err_t err = app_agent_handle_text(text);
    printf("agent: %s\n", err == ESP_OK ? "queued — outcome in the log and on the panel"
                                        : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t app_agent_init(void)
{
    /* A public fallback resolver in lwIP's second DNS slot: the router's DNS
     * answers LAN names, and when it stalls on public ones (seen as
     * getaddrinfo 202), the retry lands here instead of failing the agent. */
    {
        ip_addr_t dns2 = IPADDR4_INIT_BYTES(8, 8, 8, 8);
        dns_setserver(1, &dns2);
    }

    nvs_handle_t h;
    if (nvs_open(AGENT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n;
        n = sizeof(s_stt_key); nvs_get_str(h, "stt_key", s_stt_key, &n);
        n = sizeof(s_llm_key); nvs_get_str(h, "llm_key", s_llm_key, &n);
        n = sizeof(s_model);   nvs_get_str(h, "model", s_model, &n);
        n = sizeof(s_llm_url); nvs_get_str(h, "llm_url", s_llm_url, &n);
        n = sizeof(s_stt_url); nvs_get_str(h, "stt_url", s_stt_url, &n);
        nvs_close(h);
    }

    if (s_req_q == NULL) {
        s_req_q = xQueueCreate(1, sizeof(agent_req_t));
        /* 20 KB: TLS handshake + JSON of a few tool turns + the Lua sandbox.
         * Priority below the audio tasks — the agent waits on the network
         * most of its life and must never starve the recogniser. */
        /* The stack must NOT land in the RTC-RAM heap pool: plain xTaskCreate
         * once placed these 20 KB there (it fits the 31 KB pool) and the task
         * crashed with its SP at 0x2e00xxxx mid-DNS — a wild jump from a
         * corrupted frame. Internal DRAM has no contiguous 20 KB left on this
         * build, so the stack lives in PSRAM (allowed by
         * SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY, fine for a network-bound task). */
        if (s_req_q == NULL ||
            xTaskCreateWithCaps(agent_task, "agent", 20480, NULL, 3, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGE(TAG, "agent task/queue not created — agent disabled");
        }
    }

    esp_console_cmd_t c = {};
    c.command = "agent-key-stt";
    c.help = "Store the STT (Whisper) bearer key";
    c.func = &cmd_key_stt;
    esp_console_cmd_register(&c);
    c = {};
    c.command = "agent-key-llm";
    c.help = "Store the chat endpoint bearer key";
    c.func = &cmd_key_llm;
    esp_console_cmd_register(&c);
    c = {};
    c.command = "agent-model";
    c.help = "Show agent config, or set the model id";
    c.func = &cmd_model;
    esp_console_cmd_register(&c);
    c = {};
    c.command = "agent-url";
    c.help = "Show or set the chat-completions endpoint URL";
    c.func = &cmd_url;
    esp_console_cmd_register(&c);
    c = {};
    c.command = "agent-test";
    c.help = "Run the agent on typed text: agent-test set the office to 21";
    c.func = &cmd_test;
    esp_console_cmd_register(&c);

    ESP_LOGI(TAG, "agent %s (model %s)",
             app_agent_configured() ? "configured" : "waiting for keys "
             "(agent-key-stt / agent-key-llm)", s_model);
    return ESP_OK;
}
